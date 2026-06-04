/**
 * @file connection.hpp
 * @brief 连接管理器 - 高效连接池和超时管理
 */
#ifndef CONNECTION_HPP
#define CONNECTION_HPP

#include "memory_pool.hpp"
#include "timer.hpp"
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

namespace high_perf {

/**
 * @brief 文件描述符 RAII 封装
 */
class ScopedFD {
public:
    ScopedFD() : fd_(-1) {}
    explicit ScopedFD(int fd) : fd_(fd) {}

    ~ScopedFD() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    ScopedFD(ScopedFD&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    ScopedFD& operator=(ScopedFD&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // 禁止拷贝
    ScopedFD(const ScopedFD&) = delete;
    ScopedFD& operator=(const ScopedFD&) = delete;

    int get() const { return fd_; }
    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) {
        if (fd_ >= 0 && fd_ != fd) {
            ::close(fd_);
        }
        fd_ = fd;
    }
    bool valid() const { return fd_ >= 0; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_;
};

/**
 * @brief 缓冲区
 */
class Buffer {
public:
    static constexpr size_t DEFAULT_SIZE = 4096;

    explicit Buffer(size_t cap = DEFAULT_SIZE)
        : capacity_(cap), size_(0), data_(new char[cap]) {}

    ~Buffer() { delete[] data_; }

    Buffer(Buffer&& other) noexcept
        : capacity_(other.capacity_), size_(other.size_), data_(other.data_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            delete[] data_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            data_ = other.data_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    char* data() { return data_; }
    const char* data() const { return data_; }
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }

    void resize(size_t new_size) {
        if (new_size > capacity_) {
            reserve(new_size * 2);
        }
        size_ = new_size;
    }

    void reserve(size_t new_cap) {
        if (new_cap > capacity_) {
            char* new_data = new char[new_cap];
            if (data_) {
                memcpy(new_data, data_, size_);
                delete[] data_;
            }
            data_ = new_data;
            capacity_ = new_cap;
        }
    }

    void append(const void* data, size_t len) {
        if (size_ + len > capacity_) {
            reserve((size_ + len) * 2);
        }
        memcpy(data_ + size_, data, len);
        size_ += len;
    }

    void clear() { size_ = 0; }

    ssize_t read_from_fd(int fd) {
        // 剩余空间不足则扩展
        if (size_ + 1024 > capacity_) {
            reserve(capacity_ * 2);
        }
        ssize_t n = ::read(fd, data_ + size_, capacity_ - size_ - 1);
        if (n > 0) {
            size_ += n;
        }
        return n;
    }

    ssize_t write_to_fd(int fd) {
        ssize_t n = ::write(fd, data_, size_);
        if (n > 0) {
            memmove(data_, data_ + n, size_ - n);
            size_ -= n;
        }
        return n;
    }

private:
    size_t capacity_;
    size_t size_;
    char* data_;
};

/**
 * @brief HTTP 连接状态
 */
enum class ConnectionState {
    CONNECTED,
    READ_REQUEST,
    PROCESSING,
    WRITE_RESPONSE,
    KEEP_ALIVE,
    CLOSING,
    CLOSED
};

/**
 * @brief HTTP 请求结构
 */
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::string query_string;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> params;

    void clear() {
        method.clear();
        path.clear();
        version.clear();
        query_string.clear();
        headers.clear();
        params.clear();
    }
};

/**
 * @brief HTTP 响应结构
 */
struct HttpResponse {
    int status_code = 200;
    std::string status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    void set_header(const std::string& key, const std::string& val) {
        headers[key] = val;
    }

    void clear() {
        status_code = 200;
        status_message.clear();
        headers.clear();
        body.clear();
    }
};

/**
 * @brief 连接上下文
 */
struct ConnectionContext {
    int fd = -1;
    ConnectionState state = ConnectionState::CONNECTED;
    std::unique_ptr<Buffer> read_buffer;
    std::unique_ptr<Buffer> write_buffer;
    HttpRequest request;
    HttpResponse response;
    uint32_t timer_id = 0;
    int64_t last_active_time = 0;
    bool keep_alive = true;

    ConnectionContext() {
        read_buffer = std::make_unique<Buffer>();
        write_buffer = std::make_unique<Buffer>();
    }
};

/**
 * @brief 连接回调函数
 */
using ConnectionCallback = std::function<void(std::shared_ptr<ConnectionContext>)>;
using CloseCallback = std::function<void(int fd)>;

/**
 * @brief 连接管理器
 * @note 负责连接的创建、销毁、超时管理
 */
class ConnectionManager {
public:
    ConnectionManager();
    ~ConnectionManager();

    /**
     * @brief 创建新连接
     * @param fd  socket 文件描述符
     * @return 连接上下文智能指针
     */
    std::shared_ptr<ConnectionContext> create_connection(int fd);

    /**
     * @brief 关闭连接
     * @param conn 连接上下文
     */
    void close_connection(std::shared_ptr<ConnectionContext> conn);

    /**
     * @brief 关闭指定 fd 的连接
     * @param fd 文件描述符
     */
    void close_connection(int fd);

    /**
     * @brief 刷新连接活动时间
     */
    void touch_connection(std::shared_ptr<ConnectionContext> conn);

    /**
     * @brief 获取活跃连接数
     */
    size_t active_count() const;

    /**
     * @brief 遍历所有连接
     */
    void foreach (const std::function<void(std::shared_ptr<ConnectionContext>)>& cb);

    void set_close_callback(CloseCallback cb) { close_callback_ = std::move(cb); }

private:
    void check_timeout();

    std::unordered_map<int, std::shared_ptr<ConnectionContext>> connections_;
    int64_t connection_timeout_ms_;
    std::unique_ptr<MinHeapTimer> timer_;
    CloseCallback close_callback_;
    mutable std::mutex mutex_;
};

// ==================== 实现 ====================

inline ConnectionManager::ConnectionManager()
    : connection_timeout_ms_(30000)  // 30秒超时
{
    timer_ = std::make_unique<MinHeapTimer>();
}

inline ConnectionManager::~ConnectionManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [fd, conn] : connections_) {
        if (close_callback_) {
            close_callback_(fd);
        }
        ::close(fd);
    }
}

inline std::shared_ptr<ConnectionContext> ConnectionManager::create_connection(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto conn = std::make_shared<ConnectionContext>();
    conn->fd = fd;
    conn->last_active_time = MinHeapTimer::current_time_ms();

    // 添加超时定时器
    conn->timer_id = timer_->add_timer(connection_timeout_ms_, [this, fd]() {
        close_connection(fd);
    });

    connections_[fd] = conn;
    return conn;
}

inline void ConnectionManager::close_connection(std::shared_ptr<ConnectionContext> conn) {
    if (!conn) return;
    close_connection(conn->fd);
}

inline void ConnectionManager::close_connection(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(fd);
    if (it != connections_.end()) {
        auto conn = it->second;
        if (conn->timer_id != 0) {
            timer_->remove_timer(conn->timer_id);
        }
        if (close_callback_) {
            close_callback_(fd);
        }
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        connections_.erase(it);
    }
}

inline void ConnectionManager::touch_connection(std::shared_ptr<ConnectionContext> conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> lock(mutex_);
    conn->last_active_time = MinHeapTimer::current_time_ms();
}

inline size_t ConnectionManager::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

inline void ConnectionManager::foreach (
    const std::function<void(std::shared_ptr<ConnectionContext>)>& cb
) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [fd, conn] : connections_) {
        cb(conn);
    }
}

} // namespace high_perf

#endif // CONNECTION_HPP
