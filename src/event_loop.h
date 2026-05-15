#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <sys/types.h>

// 平台检测
#if defined(__linux__)
    #define RTSP_RELAY_LINUX 1
    #define RTSP_RELAY_MACOS 0
#elif defined(__APPLE__)
    #define RTSP_RELAY_LINUX 0
    #define RTSP_RELAY_MACOS 1
#else
    #error "Unsupported platform"
#endif

namespace rtsp_relay {

// 事件类型（兼容 epoll 语义）
constexpr uint32_t kEventNone  = 0;
constexpr uint32_t kEventRead  = 1 << 0;
constexpr uint32_t kEventWrite = 1 << 1;
constexpr uint32_t kEventError = 1 << 2;

class EventLoop {
public:
    using EventCallback = std::function<void(int fd, uint32_t events)>;

    EventLoop();
    ~EventLoop();

    // 不允许拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /// 运行事件循环，阻塞直到 quit()
    void loop();

    /// 退出事件循环
    void quit();

    /// 添加 fd 监听
    void addFd(int fd, uint32_t events, EventCallback cb);

    /// 修改 fd 监听事件
    void modFd(int fd, uint32_t events);

    /// 删除 fd 监听
    void delFd(int fd);

    /// 跨线程安全地投递任务到本 loop
    void runInLoop(std::function<void()> task);

    // ---------- 定时器 ----------
    using TimerCallback = std::function<void()>;

    /// 添加定时器，返回 timer_id (>0)，delay_ms 为延迟毫秒
    /// repeat=true 则周期执行
    int64_t addTimer(int64_t delay_ms, TimerCallback cb, bool repeat = false);

    /// 取消定时器
    void cancelTimer(int64_t timer_id);

    /// 当前 loop 是否在自己线程中
    bool isInLoopThread() const;

private:
    void wakeup();
    void handleWakeup();
    void processTasks();
    void processTimers();
    int  calcTimeout();

    int poll_fd_;       // epoll_fd 或 kqueue_fd
    int wakeup_fds_[2]; // [0]=read, [1]=write; Linux 可用 eventfd
    bool quit_ = false;

    std::mutex task_mutex_;
    std::vector<std::function<void()>> pending_tasks_;

    // fd -> callback 映射
    struct FdEntry {
        uint32_t events = kEventNone;
        EventCallback read_cb;
        EventCallback write_cb;
        EventCallback error_cb;
    };
    std::unordered_map<int, FdEntry> fd_entries_;

    // 定时器
    struct Timer {
        int64_t id;
        int64_t expire_ms;   // 绝对到期时间
        int64_t interval_ms; // 0=一次性
        TimerCallback cb;
    };
    std::vector<Timer> timers_;
    int64_t next_timer_id_ = 1;

    // 当前事件分发所在线程
    pthread_t thread_id_ = 0;
};

} // namespace rtsp_relay
