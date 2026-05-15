#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <sstream>
#include <vector>

#include "buffer.h"

namespace rtsp_relay {

// ==================== RtspMessage ====================
// RTSP 请求/响应消息的统一表示

enum class RtspMessageType {
    kUnknown,
    kRequest,
    kResponse,
};

enum class RtspMethod {
    kNone,
    kOptions,
    kDescribe,
    kSetup,
    kPlay,
    kPause,
    kTeardown,
    kGetParameter,
    kAnnounce,
    kRecord,
};

class RtspMessage {
public:
    RtspMessage() = default;

    // --- 请求 ---
    RtspMethod method = RtspMethod::kNone;
    std::string url;            // 请求 URI
    std::string version = "RTSP/1.0";

    // --- 响应 ---
    int status_code = 0;
    std::string reason;

    // --- 通用 ---
    RtspMessageType type = RtspMessageType::kUnknown;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    uint32_t cseq = 0;

    // --- 会话 ---
    std::string session_id;
    std::string transport;  // Transport 头部值

    // --- SDP ---
    std::string sdp;

    // ============ 构建请求 ============
    static RtspMessage optionsRequest(const std::string& url, uint32_t cseq);
    static RtspMessage describeRequest(const std::string& url, uint32_t cseq);
    static RtspMessage setupRequest(const std::string& url, uint32_t cseq,
                                     int client_rtp_port, int client_rtcp_port,
                                     const std::string& session_id = "");
    static RtspMessage playRequest(const std::string& url, uint32_t cseq,
                                    const std::string& session_id);
    static RtspMessage teardownRequest(const std::string& url, uint32_t cseq,
                                        const std::string& session_id);

    // ============ 构建响应 ============
    static RtspMessage okResponse(uint32_t cseq, const std::string& session_id = "");
    static RtspMessage optionsResponse(uint32_t cseq);
    static RtspMessage describeResponse(uint32_t cseq, const std::string& sdp,
                                         const std::string& session_id = "");
    static RtspMessage setupResponse(uint32_t cseq, const std::string& session_id,
                                      int server_rtp_port, int server_rtcp_port);
    static RtspMessage playResponse(uint32_t cseq, const std::string& session_id);
    static RtspMessage teardownResponse(uint32_t cseq, const std::string& session_id);
    static RtspMessage errorResponse(uint32_t cseq, int status, const std::string& reason);

    // 序列化为字符串
    std::string serialize() const;

    // 从缓冲区解析消息，返回是否解析成功
    // consumed 返回消耗的字节数
    static bool parse(ByteBuffer& buf, RtspMessage& msg, size_t& consumed);

    // 辅助
    static RtspMethod parseMethod(const std::string& s);
    static std::string methodToString(RtspMethod m);

private:
    static std::string generateSessionId();
};

// ==================== RtpPacket ====================
// RTP 包解析与构建

class RtpPacket {
public:
    RtpPacket() = default;

    // 从原始数据解析 RTP 头部
    bool parse(const uint8_t* data, size_t len);

    // 获取 payload 数据指针和长度
    const uint8_t* payload() const { return payload_; }
    size_t payloadSize() const { return payload_size_; }

    // RTP 头部字段
    uint8_t  version()      const { return version_; }
    bool     padding()      const { return padding_; }
    bool     extension()    const { return extension_; }
    uint8_t  csrcCount()    const { return csrc_count_; }
    bool     marker()       const { return marker_; }
    uint8_t  payloadType()  const { return payload_type_; }
    uint16_t sequenceNum()  const { return seq_; }
    uint32_t timestamp()    const { return timestamp_; }
    uint32_t ssrc()         const { return ssrc_; }

    // 完整包数据（含头部）
    const uint8_t* rawData() const { return raw_data_; }
    size_t rawSize() const { return raw_size_; }

    // 头部大小
    size_t headerSize() const { return header_size_; }

    // 构建 RTP 包
    static std::vector<uint8_t> build(uint8_t pt, bool marker,
                                       uint16_t seq, uint32_t ts, uint32_t ssrc,
                                       const uint8_t* payload, size_t payload_len);

private:
    const uint8_t* raw_data_ = nullptr;
    size_t raw_size_ = 0;
    size_t header_size_ = 0;
    const uint8_t* payload_ = nullptr;
    size_t payload_size_ = 0;

    uint8_t  version_ = 2;
    bool     padding_ = false;
    bool     extension_ = false;
    uint8_t  csrc_count_ = 0;
    bool     marker_ = false;
    uint8_t  payload_type_ = 0;
    uint16_t seq_ = 0;
    uint32_t timestamp_ = 0;
    uint32_t ssrc_ = 0;
};

} // namespace rtsp_relay
