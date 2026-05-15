#pragma once

#include "event_loop.h"
#include "buffer.h"
#include <memory>
#include <functional>
#include <string>
#include <atomic>

namespace rtsp_relay {

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using Ptr = std::shared_ptr<TcpConnection>;
    using Callback = std::function<void(const Ptr&)>;
    using DataCallback = std::function<void(const Ptr&, ByteBuffer&)>;

    TcpConnection(EventLoop* loop, int fd, const std::string& peer_addr);
    ~TcpConnection();

    // 注册读事件（需在 shared_ptr 构造完成后调用）
    void start();

    // 发送数据（线程安全）
    void send(const void* data, size_t len);
    void send(const std::string& data);
    void send(const ByteBuffer& buf);

    // 关闭连接
    void shutdown();

    // 获取信息
    int fd() const { return fd_; }
    const std::string& peerAddr() const { return peer_addr_; }
    EventLoop* getLoop() const { return loop_; }
    bool connected() const { return state_ == kConnected; }

    // 设置回调
    void setMessageCallback(DataCallback cb) { message_cb_ = std::move(cb); }
    void setWriteCompleteCallback(Callback cb) { write_complete_cb_ = std::move(cb); }
    void setCloseCallback(Callback cb) { close_cb_ = std::move(cb); }
    void setErrorCallback(Callback cb) { error_cb_ = std::move(cb); }

    // 上下文绑定（用户数据）
    void setContext(void* ctx) { context_ = ctx; }
    void* getContext() const { return context_; }

    // 启用/禁用写事件监听
    void enableWriting();
    void disableWriting();

private:
    enum State { kDisconnected, kConnecting, kConnected, kDisconnecting };

    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop(const void* data, size_t len);

    EventLoop* loop_;
    int fd_;
    std::string peer_addr_;
    std::atomic<int> state_;

    ByteBuffer input_buf_;
    ThreadSafeBuffer output_buf_; // 线程安全，跨线程写入

    DataCallback message_cb_;
    Callback write_complete_cb_;
    Callback close_cb_;
    Callback error_cb_;

    void* context_ = nullptr;
};

} // namespace rtsp_relay
