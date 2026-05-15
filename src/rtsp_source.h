#pragma once

#include "rtsp_protocol.h"
#include "tcp_client.h"
#include "event_loop.h"
#include <memory>
#include <functional>
#include <string>
#include <atomic>
#include <vector>

namespace rtsp_relay {

class RelayManager; // forward

// ==================== RtspSource ====================
// 从上游 RTSP 服务器拉流，通过 TCP interleaved 接收 RTP 数据
// 状态机：OPTIONS -> DESCRIBE -> SETUP -> PLAY -> 收流 -> TEARDOWN

class RtspSource : public std::enable_shared_from_this<RtspSource> {
public:
    using Ptr = std::shared_ptr<RtspSource>;
    using DataCallback = std::function<void(const uint8_t*, size_t)>;

    enum class State {
        kIdle,
        kConnecting,
        kOptions,
        kDescribe,
        kSetup,
        kPlaying,
        kError,
    };

    RtspSource(EventLoop* loop, const std::string& upstream_url,
               const std::string& stream_path, RelayManager* manager);
    ~RtspSource();

    void start();
    void stop();

    const std::string& streamPath() const { return stream_path_; }
    const std::string& sdp() const { return sdp_; }
    State state() const { return state_; }

    /// 注册 RTP 数据回调（由 RelayManager 调用，转发给所有 sink）
    void setDataCallback(DataCallback cb) { data_cb_ = std::move(cb); }

private:
    void connectToUpstream();
    void onConnect(const TcpConnection::Ptr& conn);
    void handleMessage(ByteBuffer& buf);
    void handleInterleavedData(uint8_t channel, const uint8_t* data, size_t len);

    // 发送 RTSP 请求
    void sendOptions();
    void sendDescribe();
    void sendSetup();
    void sendPlay();
    void sendTeardown();

    // 处理 RTSP 响应
    void handleResponse(const RtspMessage& resp);

    // 解析上游 URL
    void parseUpstreamUrl();

    EventLoop* loop_;
    std::string upstream_url_;  // 完整 URL，如 rtsp://192.168.1.100:554/stream1
    std::string stream_path_;   // 本地路径，如 /stream1
    std::string upstream_host_;
    uint16_t upstream_port_ = 554;
    std::string upstream_path_;

    RelayManager* manager_;

    std::unique_ptr<TcpClient> client_;
    TcpConnection::Ptr conn_;

    State state_ = State::kIdle;
    uint32_t cseq_ = 0;
    std::string session_id_;
    std::string sdp_;

    // RTP over TCP interleaved
    uint8_t rtp_channel_ = 0;
    uint8_t rtcp_channel_ = 1;

    DataCallback data_cb_;

    // 重连
    int64_t reconnect_timer_ = 0;
    std::atomic<bool> stopping_{false};

    // 统计
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> pkts_received_{0};

    // SDP 中解析出的视频 track 信息（用于构建转发 SDP）
    std::string video_sdp_section_;
    std::string audio_sdp_section_;
};

} // namespace rtsp_relay
