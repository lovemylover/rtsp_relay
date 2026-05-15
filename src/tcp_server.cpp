#include "tcp_server.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace rtsp_relay {

TcpServer::TcpServer(EventLoop* loop, uint16_t port, int backlog)
    : accept_loop_(loop), port_(port), backlog_(backlog) {
}

TcpServer::~TcpServer() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
}

void TcpServer::start() {
    // 创建 listen socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        abort();
    }

    int val = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));

    // 非阻塞
    int flags = ::fcntl(listen_fd_, F_GETFL, 0);
    ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "bind port %u failed: %s\n", port_, strerror(errno));
        abort();
    }

    if (::listen(listen_fd_, backlog_) < 0) {
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        abort();
    }

    printf("[TcpServer] listening on port %u, backlog=%d\n", port_, backlog_);

    // 注册 accept 事件
    accept_loop_->addFd(listen_fd_, kEventRead, [this](int, uint32_t) {
        acceptLoop();
    });
}

void TcpServer::acceptLoop() {
    while (true) {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int conn_fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer), &len);
        if (conn_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fprintf(stderr, "accept error: %s\n", strerror(errno));
            break;
        }

        char ip[64];
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        uint16_t port = ntohs(peer.sin_port);
        std::string peer_addr = std::string(ip) + ":" + std::to_string(port);

        // 设置 socket buffer
        int buf_size = 256 * 1024;
        ::setsockopt(conn_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        ::setsockopt(conn_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

        EventLoop* loop = getNextLoop();
        auto conn = std::make_shared<TcpConnection>(loop, conn_fd, peer_addr);

        auto setup = [this, conn]() {
            // conn_cb 负责设置 message/close/error 等回调
            if (conn_cb_) {
                conn_cb_(conn);
            }
            // 回调设置完毕后注册读事件（shared_from_this 可用）
            conn->start();
        };

        if (loop->isInLoopThread()) {
            setup();
        } else {
            loop->runInLoop(setup);
        }
    }
}

size_t TcpServer::addIoThread() {
    auto loop = std::make_unique<EventLoop>();
    EventLoop* raw = loop.get();
    io_loops_.push_back(std::move(loop));

    io_threads_.emplace_back([raw]() {
        raw->loop();
    });

    return io_loops_.size();
}

EventLoop* TcpServer::getNextLoop() {
    if (io_loops_.empty()) return accept_loop_;
    size_t idx = next_loop_.fetch_add(1) % io_loops_.size();
    return io_loops_[idx].get();
}

} // namespace rtsp_relay
