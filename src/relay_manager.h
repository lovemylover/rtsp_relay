#pragma once

#include "rtsp_source.h"
#include "rtsp_session.h"
#include "event_loop.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <functional>
#include <atomic>

namespace rtsp_relay {

// ==================== StreamInfo ====================
// 单条流的结构化状态信息，供 HTTP API 使用

struct StreamInfo {
    std::string stream_path;       // 本地路径
    std::string upstream_url;      // 上游源地址
    std::string source_state;      // Idle/Connecting/Options/Describe/Setup/Playing/Error
    size_t      sink_count = 0;    // 下游连接数
    std::string sdp;               // SDP（可能为空）
    bool        has_sdp = false;
};

// ==================== RelayManager ====================
// 管理所有流的上下游映射关系
// 一路流对应 1 个 RtspSource（上游拉流）+ N 个 RtspSession（下游播放）
// RTP 数据扇出：Source -> 所有 Sink

struct StreamRelay {
    RtspSource::Ptr source;
    std::string upstream_url;      // 记录上游原始 URL
    std::vector<RtspSession::Ptr> sinks;
    std::mutex sinks_mutex; // 保护 sinks 的并发插入/删除
};

class RelayManager {
public:
    RelayManager(EventLoop* loop);
    ~RelayManager();

    /// 动态添加上游流（启动拉流）
    /// 返回 true 表示新建成功，false 表示已存在
    bool addStream(const std::string& stream_path, const std::string& upstream_url);

    /// 动态移除上游流
    /// 返回 true 表示移除成功，false 表示不存在
    bool removeStream(const std::string& stream_path);

    /// 下游客户端请求播放时注册 sink
    void addSink(const std::string& stream_path, const RtspSession::Ptr& sink);

    /// 下游客户端断开时移除 sink
    void removeSink(const std::string& stream_path, const RtspSession::Ptr& sink);

    /// 获取流的 SDP
    std::string getStreamSdp(const std::string& stream_path) const;

    /// 检查流是否存在
    bool hasStream(const std::string& stream_path) const;

    /// 获取单条流详细信息
    bool getStreamInfo(const std::string& stream_path, StreamInfo& info) const;

    /// 获取所有流详细信息
    std::vector<StreamInfo> getAllStreamInfo() const;

    /// 获取所有流统计信息（文本格式，兼容旧接口）
    void getStats(std::vector<std::string>& stats) const;

    /// 并发连接数统计
    size_t totalSinks() const;

    /// 当前流数量
    size_t streamCount() const;

    /// 最大允许上游拉流路数
    size_t maxSources = 500;

private:
    void onRtpData(const std::string& stream_path, const uint8_t* data, size_t len);
    static const char* sourceStateToString(RtspSource::State s);

    EventLoop* loop_;
    mutable std::mutex streams_mutex_;
    std::unordered_map<std::string, std::unique_ptr<StreamRelay>> streams_;
};

} // namespace rtsp_relay
