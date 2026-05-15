#include "rtsp_source.h"
#include "relay_manager.h"
#include <cstdio>
#include <cstring>
#include <random>

namespace rtsp_relay {

RtspSource::RtspSource(EventLoop* loop, const std::string& upstream_url,
                         const std::string& stream_path, RelayManager* manager)
    : loop_(loop), upstream_url_(upstream_url), stream_path_(stream_path), manager_(manager) {
    parseUpstreamUrl();
}

RtspSource::~RtspSource() {
    stopping_ = true;
    printf("[RtspSource] destroyed, stream=%s, bytes=%llu, pkts=%llu\n",
           stream_path_.c_str(), (unsigned long long)bytes_received_.load(), (unsigned long long)pkts_received_.load());
}

void RtspSource::parseUpstreamUrl() {
    // rtsp://host:port/path
    size_t pos = upstream_url_.find("://");
    if (pos == std::string::npos) {
        upstream_host_ = "127.0.0.1";
        upstream_port_ = 554;
        upstream_path_ = upstream_url_;
        return;
    }

    std::string after_scheme = upstream_url_.substr(pos + 3);
    size_t slash = after_scheme.find('/');
    std::string host_port = (slash != std::string::npos) ? after_scheme.substr(0, slash) : after_scheme;
    upstream_path_ = (slash != std::string::npos) ? after_scheme.substr(slash) : "/";

    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        upstream_host_ = host_port.substr(0, colon);
        upstream_port_ = static_cast<uint16_t>(std::atoi(host_port.substr(colon + 1).c_str()));
    } else {
        upstream_host_ = host_port;
        upstream_port_ = 554;
    }
}

void RtspSource::start() {
    stopping_ = false;
    state_ = State::kConnecting;
    connectToUpstream();
}

void RtspSource::stop() {
    stopping_ = true;
    state_ = State::kIdle;

    if (conn_ && conn_->connected()) {
        sendTeardown();
    }

    client_.reset();
    conn_.reset();
}

void RtspSource::connectToUpstream() {
    if (stopping_) return;

    printf("[RtspSource] connecting to %s:%u for stream %s\n",
           upstream_host_.c_str(), upstream_port_, stream_path_.c_str());

    client_ = std::make_unique<TcpClient>(loop_, upstream_host_, upstream_port_);

    auto self = shared_from_this();
    client_->setConnectCallback([self](const TcpConnection::Ptr& conn) {
        self->onConnect(conn);
    });

    client_->setMessageCallback([self](const TcpConnection::Ptr&, ByteBuffer& buf) {
        self->handleMessage(buf);
    });

    client_->setCloseCallback([self](const TcpConnection::Ptr&) {
        printf("[RtspSource] upstream connection closed, stream=%s\n",
               self->stream_path_.c_str());
        self->conn_.reset();
        if (!self->stopping_) {
            // 3秒后重连
            self->state_ = State::kError;
            self->loop_->addTimer(3000, [self]() {
                printf("[RtspSource] reconnecting stream=%s\n", self->stream_path_.c_str());
                self->state_ = State::kConnecting;
                self->connectToUpstream();
            });
        }
    });

    client_->connect();
}

void RtspSource::onConnect(const TcpConnection::Ptr& conn) {
    conn_ = conn;
    state_ = State::kOptions;
    cseq_ = 0;
    printf("[RtspSource] connected to upstream, stream=%s\n", stream_path_.c_str());
    sendOptions();
}

void RtspSource::handleMessage(ByteBuffer& buf) {
    while (buf.readable() > 0) {
        // interleaved RTP/RTCP
        if (buf.readPtr()[0] == '$') {
            if (buf.readable() < 4) return;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.readPtr());
            uint8_t channel = p[1];
            uint16_t rtpLen = (static_cast<uint16_t>(p[2]) << 8) | p[3];
            if (buf.readable() < 4u + rtpLen) return;

            handleInterleavedData(channel, p + 4, rtpLen);
            buf.skip(4 + rtpLen);
            continue;
        }

        // RTSP 响应
        RtspMessage msg;
        size_t consumed = 0;
        if (!RtspMessage::parse(buf, msg, consumed)) return;

        buf.skip(consumed);
        handleResponse(msg);
    }
}

void RtspSource::handleInterleavedData(uint8_t channel, const uint8_t* data, size_t len) {
    bytes_received_.fetch_add(len, std::memory_order_relaxed);
    pkts_received_.fetch_add(1, std::memory_order_relaxed);

    // 直接转发给所有 sink
    if (data_cb_) {
        data_cb_(data, len);
    }
}

void RtspSource::sendOptions() {
    auto req = RtspMessage::optionsRequest(upstream_url_, ++cseq_);
    std::string data = req.serialize();
    printf("[RtspSource] >> OPTIONS %s CSeq=%u\n", upstream_url_.c_str(), cseq_);
    conn_->send(data);
}

void RtspSource::sendDescribe() {
    auto req = RtspMessage::describeRequest(upstream_url_, ++cseq_);
    std::string data = req.serialize();
    printf("[RtspSource] >> DESCRIBE %s CSeq=%u\n", upstream_url_.c_str(), cseq_);
    conn_->send(data);
}

void RtspSource::sendSetup() {
    // 使用 TCP interleaved 传输
    std::string setup_url = upstream_url_;
    // 如果 SDP 中有 control 属性，需追加，这里简化为直接用 URL

    auto req = RtspMessage::setupRequest(setup_url, ++cseq_, 0, 0, session_id_);
    req.headers["Transport"] = "RTP/AVP/TCP;unicast;interleaved=0-1";

    std::string data = req.serialize();
    printf("[RtspSource] >> SETUP %s CSeq=%u\n", setup_url.c_str(), cseq_);
    conn_->send(data);
}

void RtspSource::sendPlay() {
    auto req = RtspMessage::playRequest(upstream_url_, ++cseq_, session_id_);
    std::string data = req.serialize();
    printf("[RtspSource] >> PLAY %s CSeq=%u\n", upstream_url_.c_str(), cseq_);
    conn_->send(data);
}

void RtspSource::sendTeardown() {
    if (!conn_ || !conn_->connected()) return;
    auto req = RtspMessage::teardownRequest(upstream_url_, ++cseq_, session_id_);
    std::string data = req.serialize();
    printf("[RtspSource] >> TEARDOWN %s CSeq=%u\n", upstream_url_.c_str(), cseq_);
    conn_->send(data);
}

void RtspSource::handleResponse(const RtspMessage& resp) {
    printf("[RtspSource] << %d %s CSeq=%u\n", resp.status_code, resp.reason.c_str(), resp.cseq);

    if (resp.status_code != 200) {
        printf("[RtspSource] upstream returned error: %d %s, stream=%s\n",
               resp.status_code, resp.reason.c_str(), stream_path_.c_str());
        if (resp.status_code == 404) {
            // 流不存在，稍后重试
            loop_->addTimer(5000, [this]() {
                if (!stopping_) {
                    state_ = State::kConnecting;
                    connectToUpstream();
                }
            });
        }
        return;
    }

    // 保存 session_id（首次拿到）
    if (!resp.session_id.empty()) {
        session_id_ = resp.session_id;
    }

    switch (state_) {
    case State::kOptions:
        state_ = State::kDescribe;
        sendDescribe();
        break;

    case State::kDescribe:
        sdp_ = resp.sdp;
        state_ = State::kSetup;
        sendSetup();
        break;

    case State::kSetup: {
        // 解析 Transport 获取 interleaved 通道号
        auto it = resp.headers.find("Transport");
        if (it != resp.headers.end()) {
            const std::string& tp = it->second;
            size_t pos = tp.find("interleaved=");
            if (pos != std::string::npos) {
                size_t dash = tp.find('-', pos + 12);
                if (dash != std::string::npos) {
                    rtp_channel_ = static_cast<uint8_t>(std::atoi(tp.c_str() + pos + 12));
                    rtcp_channel_ = static_cast<uint8_t>(std::atoi(tp.c_str() + dash + 1));
                }
            }
        }
        state_ = State::kPlaying;
        sendPlay();
        break;
    }

    case State::kPlaying:
        printf("[RtspSource] streaming started, stream=%s, rtp_channel=%d\n",
               stream_path_.c_str(), rtp_channel_);
        break;

    default:
        break;
    }
}

} // namespace rtsp_relay
