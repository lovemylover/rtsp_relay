#pragma once

#include "event_loop.h"
#include "tcp_connection.h"
#include <memory>
#include <functional>
#include <string>

namespace rtsp_relay {

class TcpClient {
public:
    using Ptr = std::shared_ptr<TcpClient>;
    using ConnectCallback = std::function<void(const TcpConnection::Ptr&)>;
    using DataCallback = TcpConnection::DataCallback;

    TcpClient(EventLoop* loop, const std::string& host, uint16_t port);
    ~TcpClient();

    /// 发起非阻塞连接
    void connect();

    /// 断开并重连
    void reconnect();

    void setConnectCallback(ConnectCallback cb) { connect_cb_ = std::move(cb); }
    void setMessageCallback(DataCallback cb) { msg_cb_ = std::move(cb); }
    void setCloseCallback(TcpConnection::Callback cb) { close_cb_ = std::move(cb); }

    TcpConnection::Ptr connection() const { return conn_; }
    EventLoop* getLoop() const { return loop_; }

    const std::string& host() const { return host_; }
    uint16_t port() const { return port_; }

private:
    void handleConnect();
    void handleClose(const TcpConnection::Ptr& conn);

    EventLoop* loop_;
    std::string host_;
    uint16_t port_;
    int sock_fd_ = -1;

    TcpConnection::Ptr conn_;
    ConnectCallback connect_cb_;
    DataCallback msg_cb_;
    TcpConnection::Callback close_cb_;
};

} // namespace rtsp_relay
