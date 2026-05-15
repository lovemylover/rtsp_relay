#include "tcp_connection.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

namespace rtsp_relay {

TcpConnection::TcpConnection(EventLoop* loop, int fd, const std::string& peer_addr)
    : loop_(loop), fd_(fd), peer_addr_(peer_addr), state_(kConnected) {
    // 设置非阻塞 + TCP_NODELAY
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int val = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

    // 注意：不在构造函数中注册读事件
    // 因为此时 shared_from_this() 不可用
    // 需要在 start() 中注册，由 shared_ptr 持有回调
}

void TcpConnection::start() {
    auto self = shared_from_this();
    loop_->addFd(fd_, kEventRead, [self](int, uint32_t ev) {
        if (ev & kEventRead)  self->handleRead();
        if (ev & kEventError) self->handleError();
    });
}

TcpConnection::~TcpConnection() {
    if (fd_ >= 0) {
        loop_->delFd(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

void TcpConnection::send(const void* data, size_t len) {
    if (state_ != kConnected) return;
    if (loop_->isInLoopThread()) {
        sendInLoop(data, len);
    } else {
        // 拷贝数据跨线程投递
        std::string buf(static_cast<const char*>(data), len);
        loop_->runInLoop([this, buf = std::move(buf)]() {
            sendInLoop(buf.data(), buf.size());
        });
    }
}

void TcpConnection::send(const std::string& data) {
    send(data.data(), data.size());
}

void TcpConnection::send(const ByteBuffer& buf) {
    send(buf.readPtr(), buf.readable());
}

void TcpConnection::sendInLoop(const void* data, size_t len) {
    if (state_ != kConnected) return;

    // 先尝试直接写
    size_t remaining = len;
    ssize_t nwrote = 0;
    bool has_output = output_buf_.readable() > 0;

    if (!has_output) {
        nwrote = ::write(fd_, data, len);
        if (nwrote >= 0) {
            remaining = len - nwrote;
            if (remaining == 0) {
                // 全部写完
                if (write_complete_cb_) write_complete_cb_(shared_from_this());
                return;
            }
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                handleError();
                return;
            }
            nwrote = 0;
            remaining = len;
        }
    }

    // 剩余数据写入输出缓冲
    if (remaining > 0) {
        output_buf_.append(static_cast<const char*>(data) + nwrote, remaining);
        enableWriting();
    }
}

void TcpConnection::shutdown() {
    state_ = kDisconnecting;
    loop_->runInLoop([this]() {
        if (output_buf_.readable() == 0) {
            ::shutdown(fd_, SHUT_WR);
        }
    });
}

void TcpConnection::handleRead() {
    char buf[65536];
    while (true) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            input_buf_.append(buf, n);
            if (message_cb_) {
                message_cb_(shared_from_this(), input_buf_);
            }
        } else if (n == 0) {
            handleClose();
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            handleError();
            return;
        }
    }
}

void TcpConnection::handleWrite() {
    // 从 output_buf_ 写到 socket
    output_buf_.withBuffer([this](ByteBuffer& buf) {
        while (buf.readable() > 0) {
            ssize_t n = ::write(fd_, buf.readPtr(), buf.readable());
            if (n > 0) {
                buf.hasRead(n);
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    handleError();
                }
                break;
            }
        }
    });

    // 如果写完，禁用写事件
    if (output_buf_.readable() == 0) {
        disableWriting();
        if (write_complete_cb_) write_complete_cb_(shared_from_this());
        if (state_ == kDisconnecting) {
            ::shutdown(fd_, SHUT_WR);
        }
    }
}

void TcpConnection::handleClose() {
    state_ = kDisconnected;
    loop_->delFd(fd_);
    ::close(fd_);
    fd_ = -1;
    if (close_cb_) close_cb_(shared_from_this());
}

void TcpConnection::handleError() {
    state_ = kDisconnected;
    if (fd_ >= 0) {
        loop_->delFd(fd_);
        ::close(fd_);
        fd_ = -1;
    }
    if (error_cb_) error_cb_(shared_from_this());
}

void TcpConnection::enableWriting() {
    loop_->runInLoop([this]() {
        if (fd_ < 0) return;
        loop_->modFd(fd_, kEventRead | kEventWrite);
    });
}

void TcpConnection::disableWriting() {
    if (!loop_->isInLoopThread()) {
        loop_->runInLoop([this]() { disableWriting(); });
        return;
    }
    if (fd_ < 0) return;
    loop_->modFd(fd_, kEventRead);
}

} // namespace rtsp_relay
