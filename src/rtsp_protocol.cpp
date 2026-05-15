#include "rtsp_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <algorithm>

namespace rtsp_relay {

// ==================== RtspMessage ====================

RtspMethod RtspMessage::parseMethod(const std::string& s) {
    if (s == "OPTIONS")     return RtspMethod::kOptions;
    if (s == "DESCRIBE")    return RtspMethod::kDescribe;
    if (s == "SETUP")       return RtspMethod::kSetup;
    if (s == "PLAY")        return RtspMethod::kPlay;
    if (s == "PAUSE")       return RtspMethod::kPause;
    if (s == "TEARDOWN")    return RtspMethod::kTeardown;
    if (s == "GET_PARAMETER") return RtspMethod::kGetParameter;
    if (s == "ANNOUNCE")    return RtspMethod::kAnnounce;
    if (s == "RECORD")      return RtspMethod::kRecord;
    return RtspMethod::kNone;
}

std::string RtspMessage::methodToString(RtspMethod m) {
    switch (m) {
    case RtspMethod::kOptions:      return "OPTIONS";
    case RtspMethod::kDescribe:     return "DESCRIBE";
    case RtspMethod::kSetup:        return "SETUP";
    case RtspMethod::kPlay:         return "PLAY";
    case RtspMethod::kPause:        return "PAUSE";
    case RtspMethod::kTeardown:     return "TEARDOWN";
    case RtspMethod::kGetParameter: return "GET_PARAMETER";
    case RtspMethod::kAnnounce:     return "ANNOUNCE";
    case RtspMethod::kRecord:       return "RECORD";
    default: return "UNKNOWN";
    }
}

std::string RtspMessage::generateSessionId() {
    static std::mt19937 rng(42);
    uint32_t id = rng();
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", id);
    return buf;
}

// --- 构建请求 ---

RtspMessage RtspMessage::optionsRequest(const std::string& url, uint32_t cseq) {
    RtspMessage m;
    m.type = RtspMessageType::kRequest;
    m.method = RtspMethod::kOptions;
    m.url = url;
    m.cseq = cseq;
    m.headers["User-Agent"] = "RTSP-Relay/1.0";
    return m;
}

RtspMessage RtspMessage::describeRequest(const std::string& url, uint32_t cseq) {
    RtspMessage m;
    m.type = RtspMessageType::kRequest;
    m.method = RtspMethod::kDescribe;
    m.url = url;
    m.cseq = cseq;
    m.headers["Accept"] = "application/sdp";
    m.headers["User-Agent"] = "RTSP-Relay/1.0";
    return m;
}

RtspMessage RtspMessage::setupRequest(const std::string& url, uint32_t cseq,
                                        int client_rtp_port, int client_rtcp_port,
                                        const std::string& session_id) {
    RtspMessage m;
    m.type = RtspMessageType::kRequest;
    m.method = RtspMethod::kSetup;
    m.url = url;
    m.cseq = cseq;
    if (!session_id.empty()) m.headers["Session"] = session_id;
    char transport[256];
    snprintf(transport, sizeof(transport),
             "RTP/AVP;unicast;client_port=%d-%d", client_rtp_port, client_rtcp_port);
    m.headers["Transport"] = transport;
    m.headers["User-Agent"] = "RTSP-Relay/1.0";
    return m;
}

RtspMessage RtspMessage::playRequest(const std::string& url, uint32_t cseq,
                                      const std::string& session_id) {
    RtspMessage m;
    m.type = RtspMessageType::kRequest;
    m.method = RtspMethod::kPlay;
    m.url = url;
    m.cseq = cseq;
    m.headers["Session"] = session_id;
    m.headers["Range"] = "npt=0.000-";
    m.headers["User-Agent"] = "RTSP-Relay/1.0";
    return m;
}

RtspMessage RtspMessage::teardownRequest(const std::string& url, uint32_t cseq,
                                          const std::string& session_id) {
    RtspMessage m;
    m.type = RtspMessageType::kRequest;
    m.method = RtspMethod::kTeardown;
    m.url = url;
    m.cseq = cseq;
    m.headers["Session"] = session_id;
    m.headers["User-Agent"] = "RTSP-Relay/1.0";
    return m;
}

// --- 构建响应 ---

RtspMessage RtspMessage::okResponse(uint32_t cseq, const std::string& session_id) {
    RtspMessage m;
    m.type = RtspMessageType::kResponse;
    m.status_code = 200;
    m.reason = "OK";
    m.cseq = cseq;
    if (!session_id.empty()) m.headers["Session"] = session_id;
    return m;
}

RtspMessage RtspMessage::optionsResponse(uint32_t cseq) {
    RtspMessage m;
    m.type = RtspMessageType::kResponse;
    m.status_code = 200;
    m.reason = "OK";
    m.cseq = cseq;
    m.headers["Public"] = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER";
    return m;
}

RtspMessage RtspMessage::describeResponse(uint32_t cseq, const std::string& sdp,
                                            const std::string& session_id) {
    RtspMessage m;
    m.type = RtspMessageType::kResponse;
    m.status_code = 200;
    m.reason = "OK";
    m.cseq = cseq;
    if (!session_id.empty()) m.headers["Session"] = session_id;
    m.headers["Content-Type"] = "application/sdp";
    m.headers["Content-Base"] = "";  // 调用者需设置
    m.body = sdp;
    m.sdp = sdp;
    return m;
}

RtspMessage RtspMessage::setupResponse(uint32_t cseq, const std::string& session_id,
                                         int server_rtp_port, int server_rtcp_port) {
    RtspMessage m;
    m.type = RtspMessageType::kResponse;
    m.status_code = 200;
    m.reason = "OK";
    m.cseq = cseq;
    m.headers["Session"] = session_id + ";timeout=60";
    char transport[256];
    snprintf(transport, sizeof(transport),
             "RTP/AVP;unicast;server_port=%d-%d", server_rtp_port, server_rtcp_port);
    m.headers["Transport"] = transport;
    return m;
}

RtspMessage RtspMessage::playResponse(uint32_t cseq, const std::string& session_id) {
    RtspMessage m;
    m.type = RtspMessageType::kResponse;
    m.status_code = 200;
    m.reason = "OK";
    m.cseq = cseq;
    m.headers["Session"] = session_id;
    m.headers["Range"] = "npt=0.000-";
    return m;
}

RtspMessage RtspMessage::teardownResponse(uint32_t cseq, const std::string& session_id) {
    RtspMessage m;
    m.type = RtspMessageType::kResponse;
    m.status_code = 200;
    m.reason = "OK";
    m.cseq = cseq;
    if (!session_id.empty()) m.headers["Session"] = session_id;
    return m;
}

RtspMessage RtspMessage::errorResponse(uint32_t cseq, int status, const std::string& reason) {
    RtspMessage m;
    m.type = RtspMessageType::kResponse;
    m.status_code = status;
    m.reason = reason;
    m.cseq = cseq;
    return m;
}

// ==================== 序列化 ====================

std::string RtspMessage::serialize() const {
    std::ostringstream oss;

    if (type == RtspMessageType::kRequest) {
        oss << methodToString(method) << " " << url << " " << version << "\r\n";
    } else {
        oss << version << " " << status_code << " " << reason << "\r\n";
    }

    oss << "CSeq: " << cseq << "\r\n";

    for (auto& [key, val] : headers) {
        if (key == "CSeq") continue; // 已写
        oss << key << ": " << val << "\r\n";
    }

    if (!body.empty()) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }

    oss << "\r\n";

    if (!body.empty()) {
        oss << body;
    }

    return oss.str();
}

// ==================== 解析 ====================

bool RtspMessage::parse(ByteBuffer& buf, RtspMessage& msg, size_t& consumed) {
    consumed = 0;

    // 找到头部结束标记 \r\n\r\n
    const char* headerEndPattern = "\r\n\r\n";
    int headerEndPos = buf.find(headerEndPattern, 4);
    if (headerEndPos < 0) return false;

    size_t headerLen = headerEndPos + 4; // 含 \r\n\r\n
    std::string header(buf.readPtr(), headerLen);

    // 解析第一行
    size_t lineEnd = header.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::string firstLine = header.substr(0, lineEnd);

    if (firstLine.compare(0, 5, "RTSP/") == 0) {
        // 响应: RTSP/1.0 200 OK
        msg.type = RtspMessageType::kResponse;
        // 解析状态码
        size_t sp1 = firstLine.find(' ');
        if (sp1 == std::string::npos) return false;
        size_t sp2 = firstLine.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return false;
        msg.version = firstLine.substr(0, sp1);
        msg.status_code = std::atoi(firstLine.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
        msg.reason = firstLine.substr(sp2 + 1);
    } else {
        // 请求: METHOD URL RTSP/1.0
        msg.type = RtspMessageType::kRequest;
        size_t sp1 = firstLine.find(' ');
        if (sp1 == std::string::npos) return false;
        size_t sp2 = firstLine.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return false;
        msg.method = parseMethod(firstLine.substr(0, sp1));
        msg.url = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
        msg.version = firstLine.substr(sp2 + 1);
    }

    // 解析头部
    size_t pos = lineEnd + 2;
    while (pos < headerLen - 2) {
        size_t nextLine = header.find("\r\n", pos);
        if (nextLine == std::string::npos || nextLine == pos) break;
        std::string line = header.substr(pos, nextLine - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            // 跳过冒号后空格
            size_t valStart = colon + 1;
            while (valStart < line.size() && line[valStart] == ' ') ++valStart;
            std::string val = line.substr(valStart);
            msg.headers[key] = val;

            // 特殊字段提取
            if (key == "CSeq") msg.cseq = static_cast<uint32_t>(std::atoi(val.c_str()));
            if (key == "Session") {
                // Session 可能含 ";timeout=60"
                size_t semi = val.find(';');
                msg.session_id = (semi != std::string::npos) ? val.substr(0, semi) : val;
            }
            if (key == "Transport") msg.transport = val;
        }
        pos = nextLine + 2;
    }

    // 解析 body（根据 Content-Length）
    auto it = msg.headers.find("Content-Length");
    size_t contentLength = 0;
    if (it != msg.headers.end()) {
        contentLength = static_cast<size_t>(std::atoll(it->second.c_str()));
    }

    if (headerLen + contentLength > buf.readable()) {
        // 数据不完整，等更多数据
        return false;
    }

    if (contentLength > 0) {
        msg.body = std::string(buf.readPtr() + headerLen, contentLength);
        auto ct = msg.headers.find("Content-Type");
        if (ct != msg.headers.end() && ct->second.find("sdp") != std::string::npos) {
            msg.sdp = msg.body;
        }
    }

    consumed = headerLen + contentLength;
    return true;
}

// ==================== RtpPacket ====================

bool RtpPacket::parse(const uint8_t* data, size_t len) {
    if (len < 12) return false; // 最小 RTP 头部

    raw_data_ = data;
    raw_size_ = len;

    version_ = (data[0] >> 6) & 0x03;
    if (version_ != 2) return false;

    padding_ = (data[0] & 0x20) != 0;
    extension_ = (data[0] & 0x10) != 0;
    csrc_count_ = data[0] & 0x0F;
    marker_ = (data[1] & 0x80) != 0;
    payload_type_ = data[1] & 0x7F;
    seq_ = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    timestamp_ = (static_cast<uint32_t>(data[4]) << 24) |
                 (static_cast<uint32_t>(data[5]) << 16) |
                 (static_cast<uint32_t>(data[6]) << 8) |
                 data[7];
    ssrc_ = (static_cast<uint32_t>(data[8]) << 24) |
            (static_cast<uint32_t>(data[9]) << 16) |
            (static_cast<uint32_t>(data[10]) << 8) |
            data[11];

    header_size_ = 12 + csrc_count_ * 4;
    if (extension_) {
        if (header_size_ + 4 > len) return false;
        uint16_t ext_len = (static_cast<uint16_t>(data[header_size_ + 2]) << 8) |
                           data[header_size_ + 3];
        header_size_ += 4 + ext_len * 4;
    }

    if (header_size_ > len) return false;

    payload_ = data + header_size_;
    payload_size_ = len - header_size_;

    // padding 处理
    if (padding_ && len > 0) {
        uint8_t pad_len = data[len - 1];
        if (pad_len < payload_size_) {
            payload_size_ -= pad_len;
        }
    }

    return true;
}

std::vector<uint8_t> RtpPacket::build(uint8_t pt, bool marker,
                                        uint16_t seq, uint32_t ts, uint32_t ssrc,
                                        const uint8_t* payload, size_t payload_len) {
    std::vector<uint8_t> pkt(12 + payload_len);

    pkt[0] = 0x80; // V=2, P=0, X=0, CC=0
    pkt[1] = (marker ? 0x80 : 0x00) | (pt & 0x7F);
    pkt[2] = (seq >> 8) & 0xFF;
    pkt[3] = seq & 0xFF;
    pkt[4] = (ts >> 24) & 0xFF;
    pkt[5] = (ts >> 16) & 0xFF;
    pkt[6] = (ts >> 8) & 0xFF;
    pkt[7] = ts & 0xFF;
    pkt[8] = (ssrc >> 24) & 0xFF;
    pkt[9] = (ssrc >> 16) & 0xFF;
    pkt[10] = (ssrc >> 8) & 0xFF;
    pkt[11] = ssrc & 0xFF;

    if (payload && payload_len > 0) {
        std::memcpy(pkt.data() + 12, payload, payload_len);
    }

    return pkt;
}

} // namespace rtsp_relay
