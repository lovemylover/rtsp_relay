#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

namespace rtsp_relay {

// ==================== ByteBuffer ====================
// 自动扩容的字节缓冲区，用于 TCP 读写缓冲

class ByteBuffer {
public:
    ByteBuffer(size_t init_cap = 4096)
        : data_(init_cap), read_pos_(0), write_pos_(0) {}

    // 可读字节数
    size_t readable() const { return write_pos_ - read_pos_; }

    // 可写字节数（预留空间）
    size_t writable() const { return data_.size() - write_pos_; }

    // 读取指针
    const char* readPtr() const { return data_.data() + read_pos_; }
    char* readPtr() { return data_.data() + read_pos_; }

    // 写入指针
    char* writePtr() { return data_.data() + write_pos_; }

    // 跳过 n 字节（读操作后）
    void skip(size_t n) {
        read_pos_ += std::min(n, readable());
        compact();
    }

    // 已读 n 字节
    void hasRead(size_t n) { skip(n); }

    // 已写 n 字节
    void hasWritten(size_t n) {
        write_pos_ += std::min(n, writable());
    }

    // 确保有 cap 的可写空间
    void ensureWritable(size_t cap) {
        if (writable() >= cap) return;
        // 尝试 compact
        if (read_pos_ + writable() >= cap) {
            compact();
            return;
        }
        // 扩容
        size_t new_size = write_pos_ + cap;
        data_.resize(new_size);
    }

    // 写入数据
    void append(const void* buf, size_t len) {
        ensureWritable(len);
        std::memcpy(writePtr(), buf, len);
        write_pos_ += len;
    }

    void append(const std::string& s) {
        append(s.data(), s.size());
    }

    // 读取数据（不移动指针）
    size_t peek(void* buf, size_t len) const {
        size_t n = std::min(len, readable());
        std::memcpy(buf, readPtr(), n);
        return n;
    }

    // 读取数据并移动指针
    size_t read(void* buf, size_t len) {
        size_t n = peek(buf, len);
        skip(n);
        return n;
    }

    // 在可读数据中搜索字符串，返回位置（相对 readPtr），未找到返回 -1
    int find(const char* pattern, size_t pat_len) const {
        if (pat_len == 0 || pat_len > readable()) return -1;
        const char* start = readPtr();
        const char* end = start + readable() - pat_len + 1;
        for (const char* p = start; p < end; ++p) {
            if (std::memcmp(p, pattern, pat_len) == 0) {
                return static_cast<int>(p - start);
            }
        }
        return -1;
    }

    // 搜索换行符（\r\n），返回 \r 的位置，未找到返回 -1
    int findCRLF() const {
        int pos = find("\r\n", 2);
        return pos;
    }

    // 读取一行（含 CRLF），返回空串表示未找到完整行
    std::string readLine() {
        int pos = findCRLF();
        if (pos < 0) return "";
        std::string line(readPtr(), pos);
        skip(pos + 2); // 跳过 CRLF
        return line;
    }

    // 清空
    void clear() {
        read_pos_ = 0;
        write_pos_ = 0;
    }

    // compact：把已读数据搬走
    void compact() {
        if (read_pos_ == 0) return;
        if (read_pos_ == write_pos_) {
            read_pos_ = 0;
            write_pos_ = 0;
            return;
        }
        size_t r = readable();
        std::memmove(data_.data(), readPtr(), r);
        read_pos_ = 0;
        write_pos_ = r;
    }

private:
    std::vector<char> data_;
    size_t read_pos_;
    size_t write_pos_;
};

// ==================== RingBuffer ====================
// 环形缓冲区，用于一路流的多消费者扇出
// 写入端：source 写入 RTP 包
// 读取端：多个 sink 各自维护 read_offset 独立读取

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 2 * 1024 * 1024) // 默认 2MB
        : buffer_(capacity), capacity_(capacity), write_offset_(0) {}

    // 写入数据，返回 true 表示成功，false 表示空间不足（覆盖最旧数据仍写入）
    bool write(const void* data, size_t len) {
        if (len >= capacity_) return false; // 单包大于整个缓冲区
        // 如果剩余空间不足，覆盖旧数据（write_offset 推进，旧数据被覆盖）
        size_t new_offset = write_offset_ + len;
        if (new_offset > capacity_) {
            // 环回：先写尾部、再写头部
            size_t tail = capacity_ - write_offset_;
            std::memcpy(buffer_.data() + write_offset_, data, tail);
            std::memcpy(buffer_.data(), static_cast<const char*>(data) + tail, len - tail);
            write_offset_ = (len - tail) % capacity_;
        } else {
            std::memcpy(buffer_.data() + write_offset_, data, len);
            write_offset_ = new_offset == capacity_ ? 0 : new_offset;
        }
        return true;
    }

    // 从指定 offset 读取数据到 buf，返回实际读取的字节数
    // 如果请求的数据已被覆盖，返回 0
    size_t read(size_t& offset, void* buf, size_t len) const {
        size_t woff = write_offset_.load(std::memory_order_relaxed);
        // 计算可读字节数
        size_t available;
        if (woff >= offset) {
            available = woff - offset;
        } else {
            // 环回
            available = (capacity_ - offset) + woff;
        }
        size_t n = std::min(len, available);
        if (n == 0) return 0;

        if (offset + n <= capacity_) {
            std::memcpy(buf, buffer_.data() + offset, n);
            offset += n;
            if (offset == capacity_) offset = 0;
        } else {
            size_t tail = capacity_ - offset;
            std::memcpy(buf, buffer_.data() + offset, tail);
            std::memcpy(static_cast<char*>(buf) + tail, buffer_.data(), n - tail);
            offset = n - tail;
        }
        return n;
    }

    size_t capacity() const { return capacity_; }

private:
    std::vector<char> buffer_;
    size_t capacity_;
    std::atomic<size_t> write_offset_;
};

// ==================== 线程安全输出缓冲 ====================
// 用于跨线程写入场景（source 向多个 sink 推数据）

class ThreadSafeBuffer {
public:
    ThreadSafeBuffer(size_t init_cap = 16384) : buf_(init_cap) {}

    void append(const void* data, size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        buf_.append(data, len);
    }

    size_t read(void* data, size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        return buf_.read(data, len);
    }

    size_t readable() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buf_.readable();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        buf_.clear();
    }

    // 在锁内执行读操作
    template<typename Fn>
    void withBuffer(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(buf_);
    }

private:
    mutable std::mutex mutex_;
    ByteBuffer buf_;
};

} // namespace rtsp_relay
