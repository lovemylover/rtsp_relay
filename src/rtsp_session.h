#pragma once

#include "rtsp_protocol.h"
#include "tcp_connection.h"
#include <memory>
#include <functional>
#include <string>
#include <atomic>
#include <netinet/in.h>

namespace rtsp_relay {

class RelayManager; // forward

// ==================== RtspSession ====================
// 处理下游（播放端）RTSP 客户端连接
// 状态机：OPTIONS -> DESCRIBE -> SETUP -> PLAY -> ... -> TEARDOWN

class RtspSession : public std::enable_shared_from_this<RtspSession> {
public:
    using Ptr = std::shared_ptr<RtspSession>;

    enum class State {
        kIdle,
        kDescribe,
        kSetup,
        kPlaying,
        kTeardown,
    };

    enum class Transport {
        kTcpInterleaved,
        kUdp,
    };

    RtspSession(const TcpConnection::Ptr& conn, RelayManager* manager);
    ~RtspSession();

    void start();
    void close();

    // 推送 RTP 数据（来自上游 source）
    void onRtpData(const uint8_t* data, size_t len);

    // 获取流路径 (e.g., "/stream1")
    const std::string& streamPath() const { return stream_path_; }

    State state() const { return state_; }

private:
    void handleRtspMessage(ByteBuffer& buf);
    void handleInterleavedData(uint8_t channel, const uint8_t* data, size_t len);
    void processRequest(const RtspMessage& req);
    void sendResponse(const RtspMessage& resp);

    // 请求处理
    void handleOptions(const RtspMessage& req);
    void handleDescribe(const RtspMessage& req);
    void handleSetup(const RtspMessage& req);
    void handlePlay(const RtspMessage& req);
    void handleTeardown(const RtspMessage& req);
    void handleGetParameter(const RtspMessage& req);

    // 从 url 提取流路径
    std::string extractStreamPath(const std::string& url);

    // UDP 相关
    int createUdpSocket(uint16_t& port);
    void closeUdpSockets();

    TcpConnection::Ptr conn_;
    RelayManager* manager_;
    std::string stream_path_;
    std::string session_id_;
    State state_ = State::kIdle;
    uint32_t cseq_ = 0;

    // 传输模式
    Transport transport_ = Transport::kTcpInterleaved;

    // RTP over RTSP TCP interleaved 通道
    uint8_t rtp_channel_ = 0;
    uint8_t rtcp_channel_ = 1;

    // RTP over UDP
    int udp_rtp_fd_ = -1;
    int udp_rtcp_fd_ = -1;
    uint16_t server_rtp_port_ = 0;
    uint16_t server_rtcp_port_ = 0;
    struct sockaddr_in client_rtp_addr_ = {};
    struct sockaddr_in client_rtcp_addr_ = {};

    // 统计
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> pkts_sent_{0};
};

} // namespace rtsp_relay
