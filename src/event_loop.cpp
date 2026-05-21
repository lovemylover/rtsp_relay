#include "event_loop.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <thread>

#if RTSP_RELAY_LINUX
#include <sys/epoll.h>
#include <sys/eventfd.h>
#elif RTSP_RELAY_MACOS
#include <sys/event.h>
#endif

namespace rtsp_relay {

static int64_t nowMs() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp).count();
}

// ==================== EventLoop ====================

EventLoop::EventLoop() {
    thread_id_ = pthread_self(); // 将在 loop() 中更新为真实线程 id

#if RTSP_RELAY_LINUX
    poll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (poll_fd_ < 0) {
        fprintf(stderr, "epoll_create1 failed: %s\n", strerror(errno));
        abort();
    }
    // eventfd 做唤醒
    wakeup_fds_[0] = wakeup_fds_[1] = -1;
    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        fprintf(stderr, "eventfd failed: %s\n", strerror(errno));
        abort();
    }
    wakeup_fds_[0] = efd;
    wakeup_fds_[1] = efd; // eventfd 读写同一个 fd
#else
    // macOS: kqueue
    poll_fd_ = ::kqueue();
    if (poll_fd_ < 0) {
        fprintf(stderr, "kqueue failed: %s\n", strerror(errno));
        abort();
    }
    // pipe 做唤醒
    int p[2];
    if (::pipe(p) < 0) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        abort();
    }
    ::fcntl(p[0], F_SETFL, ::fcntl(p[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(p[1], F_SETFL, ::fcntl(p[1], F_GETFL) | O_NONBLOCK);
    wakeup_fds_[0] = p[0];
    wakeup_fds_[1] = p[1];
#endif

    // 注册 wakeup fd 的读事件
    addFd(wakeup_fds_[0], kEventRead, [this](int, uint32_t) {
        handleWakeup();
    });
}

EventLoop::~EventLoop() {
    ::close(poll_fd_);
    if (wakeup_fds_[0] >= 0) ::close(wakeup_fds_[0]);
#if !RTSP_RELAY_LINUX
    if (wakeup_fds_[1] >= 0 && wakeup_fds_[1] != wakeup_fds_[0]) ::close(wakeup_fds_[1]);
#endif
}

void EventLoop::loop() {
    thread_id_ = pthread_self();
    quit_ = false;

#if RTSP_RELAY_LINUX
    const int kMaxEvents = 1024;
    struct epoll_event events[kMaxEvents];

    while (!quit_) {
        processTasks();
        processTimers();
        int timeout = calcTimeout();
        int n = ::epoll_wait(poll_fd_, events, kMaxEvents, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "epoll_wait error: %s\n", strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            auto it = fd_entries_.find(fd);
            if (it == fd_entries_.end()) continue;

            uint32_t ev = events[i].events;
            if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                if (it->second.read_cb) it->second.read_cb(fd, kEventRead);
            }
            if (ev & EPOLLOUT) {
                if (it->second.write_cb) it->second.write_cb(fd, kEventWrite);
            }
        }
    }
#else
    const int kMaxEvents = 1024;
    struct kevent events[kMaxEvents];

    while (!quit_) {
        processTasks();
        processTimers();
        int timeout = calcTimeout();
        struct timespec ts;
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000) * 1000000;
        int n = ::kevent(poll_fd_, nullptr, 0, events, kMaxEvents,
                         timeout >= 0 ? &ts : nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "kevent error: %s\n", strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            int evfd = static_cast<int>(events[i].ident);
            auto it = fd_entries_.find(evfd);
            if (it == fd_entries_.end()) continue;

            int16_t filt = events[i].filter;
            uint32_t flags = events[i].flags;

            if (filt == EVFILT_READ || (flags & EV_EOF)) {
                if (it->second.read_cb) it->second.read_cb(evfd, kEventRead);
            }
            if (filt == EVFILT_WRITE) {
                if (it->second.write_cb) it->second.write_cb(evfd, kEventWrite);
            }
        }
    }
#endif
}

void EventLoop::quit() {
    quit_ = true;
    wakeup();
}

void EventLoop::addFd(int fd, uint32_t events, EventCallback cb) {
    FdEntry entry;
    entry.events = events;
    if (events & kEventRead)  entry.read_cb  = cb;
    if (events & kEventWrite) entry.write_cb = cb;
    if (events & kEventError) entry.error_cb  = cb;
    fd_entries_[fd] = entry;

#if RTSP_RELAY_LINUX
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    ev.events = 0;
    if (events & kEventRead)  ev.events |= EPOLLIN;
    if (events & kEventWrite) ev.events |= EPOLLOUT;
    ::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev);
#else
    struct kevent ev;
    if (events & kEventRead) {
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (::kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
            fprintf(stderr, "[EventLoop] kevent ADD READ fd=%d failed: %s\n", fd, strerror(errno));
        }
    }
    if (events & kEventWrite) {
        EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ::kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);
    }
#endif
}

void EventLoop::modFd(int fd, uint32_t events) {
    auto it = fd_entries_.find(fd);
    if (it == fd_entries_.end()) return;

    it->second.events = events;
    // 保留已有回调，仅增减类型
    if ((events & kEventRead) && !it->second.read_cb) {
        it->second.read_cb = it->second.error_cb; // fallback
    }

#if RTSP_RELAY_LINUX
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    ev.events = 0;
    if (events & kEventRead)  ev.events |= EPOLLIN;
    if (events & kEventWrite) ev.events |= EPOLLOUT;
    ::epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev);
#else
    struct kevent ch[2];
    int n = 0;
    if (events & kEventRead) {
        EV_SET(&ch[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    } else {
        EV_SET(&ch[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if (events & kEventWrite) {
        EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    } else {
        // 只在原先有写事件时才删除
        EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    ::kevent(poll_fd_, ch, n, nullptr, 0, nullptr);
#endif
}

void EventLoop::delFd(int fd) {
    fd_entries_.erase(fd);

#if RTSP_RELAY_LINUX
    struct epoll_event ev;
    ::epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, &ev);
#else
    struct kevent ch[2];
    EV_SET(&ch[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(poll_fd_, ch, 2, nullptr, 0, nullptr);
#endif
}

void EventLoop::runInLoop(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        pending_tasks_.push_back(std::move(task));
    }
    wakeup();
}

void EventLoop::wakeup() {
#if RTSP_RELAY_LINUX
    uint64_t one = 1;
    ssize_t n = ::write(wakeup_fds_[1], &one, sizeof(one));
    (void)n;
#else
    char c = 'w';
    ssize_t n = ::write(wakeup_fds_[1], &c, 1);
    (void)n;
#endif
}

void EventLoop::handleWakeup() {
#if RTSP_RELAY_LINUX
    uint64_t val;
    while (true) {
        ssize_t n = ::read(wakeup_fds_[0], &val, sizeof(val));
        if (n < 0) break;
    }
#else
    char buf[256];
    while (true) {
        ssize_t n = ::read(wakeup_fds_[0], buf, sizeof(buf));
        if (n <= 0) break;
    }
#endif
}

void EventLoop::processTasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        tasks.swap(pending_tasks_);
    }
    for (auto& t : tasks) {
        t();
    }
}

// ---------- 定时器 ----------

int64_t EventLoop::addTimer(int64_t delay_ms, TimerCallback cb, bool repeat) {
    Timer t;
    t.id = next_timer_id_++;
    t.expire_ms = nowMs() + delay_ms;
    t.interval_ms = repeat ? delay_ms : 0;
    t.cb = std::move(cb);
    timers_.push_back(std::move(t));
    return timers_.back().id;
}

void EventLoop::cancelTimer(int64_t timer_id) {
    auto it = std::remove_if(timers_.begin(), timers_.end(),
        [timer_id](const Timer& t) { return t.id == timer_id; });
    timers_.erase(it, timers_.end());
}

void EventLoop::processTimers() {
    int64_t now = nowMs();
    std::vector<Timer> expired;

    // 取出到期的定时器
    auto it = std::partition(timers_.begin(), timers_.end(),
        [now](const Timer& t) { return t.expire_ms > now; });
    expired.assign(it, timers_.end());
    timers_.erase(it, timers_.end());

    // 执行到期回调，周期定时器重新入队
    for (auto& t : expired) {
        if (t.cb) t.cb();
        if (t.interval_ms > 0) {
            t.expire_ms = now + t.interval_ms;
            timers_.push_back(std::move(t));
        }
    }
}

int EventLoop::calcTimeout() {
    if (timers_.empty()) return 100; // 默认 100ms
    int64_t now = nowMs();
    int64_t nearest = timers_[0].expire_ms;
    for (const auto& t : timers_) {
        if (t.expire_ms < nearest) nearest = t.expire_ms;
    }
    int64_t diff = nearest - now;
    if (diff <= 0) return 0;
    return static_cast<int>(diff > 1000 ? 1000 : diff);
}

bool EventLoop::isInLoopThread() const {
    return pthread_self() == thread_id_;
}

} // namespace rtsp_relay
