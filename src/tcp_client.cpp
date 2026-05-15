#include "tcp_client.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>

namespace rtsp_relay {

TcpClient::TcpClient(EventLoop* loop, const std::string& host, uint16_t port)
    : loop_(loop), host_(host), port_(port) {
}

TcpClient::~TcpClient() {
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
    }
}

void TcpClient::connect() {
    // 解析地址
    struct addrinfo hints, *result = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port_);
    int rc = ::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        fprintf(stderr, "[TcpClient] getaddrinfo %s:%u failed: %s\n",
                host_.c_str(), port_, gai_strerror(rc));
        // 1秒后重连
        loop_->runInLoop([this]() {
            loop_->addTimer(1000, [this]() { connect(); });
        });
        return;
    }

    sock_fd_ = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock_fd_ < 0) {
        ::freeaddrinfo(result);
        fprintf(stderr, "[TcpClient] socket failed: %s\n", strerror(errno));
        loop_->addTimer(1000, [this]() { connect(); });
        return;
    }

    // 非阻塞
    int flags = ::fcntl(sock_fd_, F_GETFL, 0);
    ::fcntl(sock_fd_, F_SETFL, flags | O_NONBLOCK);

    // TCP_NODELAY
    int val = 1;
    ::setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

    // 发起连接
    int ret = ::connect(sock_fd_, result->ai_addr, result->ai_addrlen);
    ::freeaddrinfo(result);

    if (ret == 0) {
        // 立即连上（罕见）
        handleConnect();
    } else if (errno == EINPROGRESS) {
        // 等待可写
        loop_->addFd(sock_fd_, kEventWrite, [this](int fd, uint32_t ev) {
            (void)ev;
            loop_->delFd(fd);
            // 检查连接结果
            int err = 0;
            socklen_t len = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) {
                fprintf(stderr, "[TcpClient] connect to %s:%u failed: %s\n",
                        host_.c_str(), port_, strerror(err));
                ::close(sock_fd_);
                sock_fd_ = -1;
                loop_->addTimer(1000, [this]() { connect(); });
                return;
            }
            handleConnect();
        });
    } else {
        fprintf(stderr, "[TcpClient] connect to %s:%u failed: %s\n",
                host_.c_str(), port_, strerror(errno));
        ::close(sock_fd_);
        sock_fd_ = -1;
        loop_->addTimer(1000, [this]() { connect(); });
    }
}

void TcpClient::handleConnect() {
    // 连接成功，创建 TcpConnection
    std::string peer_addr = host_ + ":" + std::to_string(port_);
    conn_ = std::make_shared<TcpConnection>(loop_, sock_fd_, peer_addr);
    sock_fd_ = -1; // ownership 转移给 TcpConnection

    conn_->setCloseCallback([this](const TcpConnection::Ptr& c) {
        handleClose(c);
    });

    if (msg_cb_) {
        conn_->setMessageCallback(msg_cb_);
    }

    if (connect_cb_) {
        connect_cb_(conn_);
    }

    // 启动读事件（shared_from_this 可用）
    conn_->start();
}

void TcpClient::handleClose(const TcpConnection::Ptr& conn) {
    if (close_cb_) close_cb_(conn);
    conn_.reset();
}

void TcpClient::reconnect() {
    conn_.reset();
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
    loop_->addTimer(1000, [this]() { connect(); });
}

} // namespace rtsp_relay
