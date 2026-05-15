#include "rtsp_session.h"
#include "relay_manager.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace rtsp_relay {

static std::string generateSessionId() {
    static std::mt19937 rng(std::random_device{}());
    uint32_t id = rng();
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", id);
    return buf;
}

RtspSession::RtspSession(const TcpConnection::Ptr& conn, RelayManager* manager)
    : conn_(conn), manager_(manager) {
    session_id_ = generateSessionId();
}

RtspSession::~RtspSession() {
    closeUdpSockets();
    printf("[RtspSession] destroyed, stream=%s, bytes_sent=%llu, pkts_sent=%llu\n",
           stream_path_.c_str(), (unsigned long long)bytes_sent_.load(), (unsigned long long)pkts_sent_.load());
}

void RtspSession::start() {
    auto self = shared_from_this();
    conn_->setMessageCallback([self](const TcpConnection::Ptr&, ByteBuffer& buf) {
        self->handleRtspMessage(buf);
    });

    conn_->setCloseCallback([self](const TcpConnection::Ptr&) {
        self->close();
    });

    conn_->setErrorCallback([self](const TcpConnection::Ptr&) {
        self->close();
    });
}

void RtspSession::close() {
    if (state_ == State::kTeardown) return;

    State old = state_;
    state_ = State::kTeardown;

    if (old == State::kPlaying && !stream_path_.empty()) {
        manager_->removeSink(stream_path_, shared_from_this());
    }

    closeUdpSockets();
    printf("[RtspSession] closed, stream=%s\n", stream_path_.c_str());
}

void RtspSession::handleRtspMessage(ByteBuffer& buf) {
    while (buf.readable() > 0) {
        // 检查是否是 interleaved RTP/RTCP 数据 ('$' 开头)
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

        // 尝试解析 RTSP 消息
        RtspMessage msg;
        size_t consumed = 0;
        if (!RtspMessage::parse(buf, msg, consumed)) {
            return;
        }

        buf.skip(consumed);
        processRequest(msg);
    }
}

void RtspSession::handleInterleavedData(uint8_t channel, const uint8_t* data, size_t len) {
    (void)channel;
    (void)data;
    (void)len;
}

void RtspSession::processRequest(const RtspMessage& req) {
    if (req.type == RtspMessageType::kRequest) {
        cseq_ = req.cseq;
        printf("[RtspSession] << %s %s CSeq=%u\n",
               RtspMessage::methodToString(req.method).c_str(),
               req.url.c_str(), req.cseq);

        switch (req.method) {
        case RtspMethod::kOptions:      handleOptions(req); break;
        case RtspMethod::kDescribe:     handleDescribe(req); break;
        case RtspMethod::kSetup:        handleSetup(req); break;
        case RtspMethod::kPlay:         handlePlay(req); break;
        case RtspMethod::kTeardown:     handleTeardown(req); break;
        case RtspMethod::kGetParameter: handleGetParameter(req); break;
        default:
            sendResponse(RtspMessage::errorResponse(req.cseq, 461, "Not Implemented"));
            break;
        }
    }
}

void RtspSession::handleOptions(const RtspMessage& req) {
    auto resp = RtspMessage::optionsResponse(req.cseq);
    resp.headers["Session"] = session_id_;
    sendResponse(resp);
}

void RtspSession::handleDescribe(const RtspMessage& req) {
    stream_path_ = extractStreamPath(req.url);
    state_ = State::kDescribe;

    std::string sdp = manager_->getStreamSdp(stream_path_);
    if (sdp.empty()) {
        sendResponse(RtspMessage::errorResponse(req.cseq, 404, "Stream Not Found"));
        return;
    }

    std::string base_url = req.url;
    auto resp = RtspMessage::describeResponse(req.cseq, sdp, session_id_);
    resp.headers["Content-Base"] = base_url + "/";
    sendResponse(resp);
}

void RtspSession::handleSetup(const RtspMessage& req) {
    if (stream_path_.empty()) {
        stream_path_ = extractStreamPath(req.url);
    }
    state_ = State::kSetup;

    auto it = req.headers.find("Transport");
    if (it == req.headers.end()) {
        sendResponse(RtspMessage::errorResponse(req.cseq, 461, "Transport header required"));
        return;
    }

    const std::string& tp = it->second;

    // 判断客户端请求的传输方式
    bool client_wants_tcp = (tp.find("interleaved=") != std::string::npos);
    bool client_wants_udp = (tp.find("client_port=") != std::string::npos);

    if (client_wants_tcp) {
        // TCP interleaved 模式
        transport_ = Transport::kTcpInterleaved;

        size_t pos = tp.find("interleaved=");
        if (pos != std::string::npos) {
            size_t dash = tp.find('-', pos + 12);
            if (dash != std::string::npos) {
                rtp_channel_ = static_cast<uint8_t>(std::atoi(tp.c_str() + pos + 12));
                rtcp_channel_ = static_cast<uint8_t>(std::atoi(tp.c_str() + dash + 1));
            }
        }

        auto resp = RtspMessage::setupResponse(req.cseq, session_id_, 0, 0);
        resp.headers["Transport"] = "RTP/AVP/TCP;unicast;interleaved=" +
            std::to_string(rtp_channel_) + "-" + std::to_string(rtcp_channel_);
        sendResponse(resp);

        printf("[RtspSession] SETUP stream=%s, transport=TCP interleaved, channels=%d/%d\n",
               stream_path_.c_str(), rtp_channel_, rtcp_channel_);

    } else if (client_wants_udp) {
        // UDP 模式
        transport_ = Transport::kUdp;

        // 解析客户端端口：client_port=xxx-yyy
        uint16_t client_rtp_port = 0;
        uint16_t client_rtcp_port = 0;
        size_t cpos = tp.find("client_port=");
        if (cpos != std::string::npos) {
            size_t dash = tp.find('-', cpos + 12);
            if (dash != std::string::npos) {
                client_rtp_port = static_cast<uint16_t>(std::atoi(tp.c_str() + cpos + 12));
                client_rtcp_port = static_cast<uint16_t>(std::atoi(tp.c_str() + dash + 1));
            }
        }

        if (client_rtp_port == 0) {
            sendResponse(RtspMessage::errorResponse(req.cseq, 461, "Bad Transport"));
            return;
        }

        // 创建 UDP socket（自动分配端口）
        udp_rtp_fd_ = createUdpSocket(server_rtp_port_);
        udp_rtcp_fd_ = createUdpSocket(server_rtcp_port_);

        if (udp_rtp_fd_ < 0 || udp_rtcp_fd_ < 0) {
            closeUdpSockets();
            sendResponse(RtspMessage::errorResponse(req.cseq, 500, "UDP socket creation failed"));
            return;
        }

        // 获取客户端 IP（从 TCP 连接的 peer 地址）
        std::string peer = conn_->peerAddr();
        size_t colon = peer.rfind(':');
        std::string client_ip = (colon != std::string::npos) ? peer.substr(0, colon) : peer;

        // 构造客户端 RTP/RTCP 地址
        memset(&client_rtp_addr_, 0, sizeof(client_rtp_addr_));
        client_rtp_addr_.sin_family = AF_INET;
        client_rtp_addr_.sin_port = htons(client_rtp_port);
        inet_pton(AF_INET, client_ip.c_str(), &client_rtp_addr_.sin_addr);

        memset(&client_rtcp_addr_, 0, sizeof(client_rtcp_addr_));
        client_rtcp_addr_.sin_family = AF_INET;
        client_rtcp_addr_.sin_port = htons(client_rtcp_port);
        client_rtcp_addr_.sin_addr = client_rtp_addr_.sin_addr;

        auto resp = RtspMessage::setupResponse(req.cseq, session_id_,
                                                  server_rtp_port_, server_rtcp_port_);
        sendResponse(resp);

        printf("[RtspSession] SETUP stream=%s, transport=UDP, server=%u/%u, client=%s:%u/%u\n",
               stream_path_.c_str(), server_rtp_port_, server_rtcp_port_,
               client_ip.c_str(), client_rtp_port, client_rtcp_port);

    } else {
        sendResponse(RtspMessage::errorResponse(req.cseq, 461, "Unsupported Transport"));
    }
}

void RtspSession::handlePlay(const RtspMessage& req) {
    state_ = State::kPlaying;

    manager_->addSink(stream_path_, shared_from_this());

    auto resp = RtspMessage::playResponse(req.cseq, session_id_);
    resp.headers["RTP-Info"] = "url=" + req.url + ";seq=0;rtptime=0";
    sendResponse(resp);

    printf("[RtspSession] PLAY stream=%s, transport=%s\n",
           stream_path_.c_str(),
           transport_ == Transport::kUdp ? "UDP" : "TCP");
}

void RtspSession::handleTeardown(const RtspMessage& req) {
    auto resp = RtspMessage::teardownResponse(req.cseq, session_id_);
    sendResponse(resp);
    close();
}

void RtspSession::handleGetParameter(const RtspMessage& req) {
    auto resp = RtspMessage::okResponse(req.cseq, session_id_);
    sendResponse(resp);
}

void RtspSession::sendResponse(const RtspMessage& resp) {
    std::string data = resp.serialize();
    printf("[RtspSession] >> RTSP/1.0 %d %s CSeq=%u\n",
           resp.status_code, resp.reason.c_str(), resp.cseq);
    conn_->send(data);
}

void RtspSession::onRtpData(const uint8_t* data, size_t len) {
    if (state_ != State::kPlaying) return;

    if (transport_ == Transport::kUdp) {
        // UDP 发送
        if (udp_rtp_fd_ >= 0) {
            ssize_t n = ::sendto(udp_rtp_fd_, data, len, 0,
                                  reinterpret_cast<const struct sockaddr*>(&client_rtp_addr_),
                                  sizeof(client_rtp_addr_));
            if (n > 0) {
                bytes_sent_.fetch_add(n, std::memory_order_relaxed);
                pkts_sent_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else {
        // TCP interleaved 发送
        if (!conn_->connected()) return;

        uint8_t header[4];
        header[0] = '$';
        header[1] = rtp_channel_;
        header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>(len & 0xFF);

        conn_->send(header, 4);
        conn_->send(data, len);

        bytes_sent_.fetch_add(len + 4, std::memory_order_relaxed);
        pkts_sent_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::string RtspSession::extractStreamPath(const std::string& url) {
    size_t pos = url.find("://");
    if (pos == std::string::npos) {
        return url;
    }
    size_t slash = url.find('/', pos + 3);
    if (slash == std::string::npos) return "/";
    return url.substr(slash);
}

int RtspSession::createUdpSocket(uint16_t& port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    // 非阻塞
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);

    // 发送缓冲区
    int buf_size = 256 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    // 绑定，端口自动分配
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(0); // 自动分配端口

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    // 获取实际分配的端口
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);

    return fd;
}

void RtspSession::closeUdpSockets() {
    if (udp_rtp_fd_ >= 0) {
        ::close(udp_rtp_fd_);
        udp_rtp_fd_ = -1;
    }
    if (udp_rtcp_fd_ >= 0) {
        ::close(udp_rtcp_fd_);
        udp_rtcp_fd_ = -1;
    }
}

} // namespace rtsp_relay
