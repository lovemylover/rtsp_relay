#pragma once

#include "event_loop.h"
#include "tcp_connection.h"
#include <vector>
#include <memory>
#include <functional>
#include <thread>

namespace rtsp_relay {

class TcpServer {
public:
    using Ptr = std::shared_ptr<TcpServer>;
    using ConnectionCallback = std::function<void(const TcpConnection::Ptr&)>;
    using DataCallback = TcpConnection::DataCallback;

    TcpServer(EventLoop* loop, uint16_t port, int backlog = 512);
    ~TcpServer();

    void start();

    void setConnectionCallback(ConnectionCallback cb) { conn_cb_ = std::move(cb); }
    void setMessageCallback(DataCallback cb) { msg_cb_ = std::move(cb); }

    /// 添加 IO 线程（事件循环），返回线程数量
    size_t addIoThread();

    EventLoop* getNextLoop();

private:
    void acceptLoop();

    EventLoop* accept_loop_;
    std::vector<std::unique_ptr<EventLoop>> io_loops_;
    std::vector<std::thread> io_threads_;
    std::atomic<size_t> next_loop_{0};

    int listen_fd_ = -1;
    uint16_t port_;
    int backlog_;

    ConnectionCallback conn_cb_;
    DataCallback msg_cb_;
};

} // namespace rtsp_relay
