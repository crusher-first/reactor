/**
 * @file zero_copy.hpp
 * @brief 零拷贝文件传输 - 使用 sendfile 系统调用
 */
#ifndef ZERO_COPY_HPP
#define ZERO_COPY_HPP

#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace high_perf {

/**
 * @brief 文件信息缓存
 */
struct FileInfo {
    int fd = -1;               // 文件描述符
    struct stat st;            // 文件状态
    int64_t last_used;        // 最后使用时间
    off_t file_size;          // 文件大小

    FileInfo() = default;
    FileInfo(int f, const struct stat& s, int64_t t)
        : fd(f), st(s), last_used(t), file_size(s.st_size) {}
};

/**
 * @brief 文件缓存项
 */
struct CachedFile {
    std::string path;
    int fd;
    struct stat st;
    int64_t last_used;
    off_t file_size;
};

/**
 * @brief 零拷贝文件传输器
 * @note 使用 sendfile 系统调用，内核直接传输，避免用户态缓冲
 * @note 可减少 2 次内存拷贝
 */
class ZeroCopyFileTransfer {
public:
    /**
     * @param cache_size 文件缓存数量上限
     */
    explicit ZeroCopyFileTransfer(size_t cache_size = 100);
    ~ZeroCopyFileTransfer();

    /**
     * @brief 发送文件（零拷贝）
     * @param client_fd  客户端 socket fd
     * @param file_path  文件路径
     * @return 发送字节数，-1 表示失败
     */
    ssize_t sendfile(int client_fd, const std::string& file_path);

    /**
     * @brief 发送文件（支持偏移量和长度）
     */
    ssize_t sendfile_range(int client_fd, const std::string& file_path,
                           off_t offset, size_t length);

    /**
     * @brief 检查文件是否存在且可读
     */
    bool file_exists(const std::string& path) const;

    /**
     * @brief 获取文件 MIME 类型
     */
    static const char* get_mime_type(const std::string& path);

    /**
     * @brief 构建 HTTP 响应头
     */
    static std::string build_response_header(int status, const std::string& status_msg,
                                             size_t content_length, const char* mime_type,
                                             bool keep_alive = true);

    /**
     * @brief 获取缓存命中率
     */
    double cache_hit_rate() const;

    /**
     * @brief 清理过期缓存
     */
    void prune_expired(int64_t max_idle_ms);

private:
    int open_file(const std::string& path);
    void close_file(int fd);
    void cache_file(int fd, const std::string& path, const struct stat& st);

    size_t cache_size_;
    std::unordered_map<std::string, CachedFile> cache_;
    std::unordered_map<int, std::string> fd_to_path_;
    mutable std::mutex mutex_;
    int64_t hits_ = 0;
    int64_t misses_ = 0;
};

// ==================== 实现 ====================

inline ZeroCopyFileTransfer::ZeroCopyFileTransfer(size_t cache_size)
    : cache_size_(cache_size) {}

inline ZeroCopyFileTransfer::~ZeroCopyFileTransfer() {
    // 关闭所有缓存的文件描述符
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [path, file] : cache_) {
        if (file.fd >= 0) {
            ::close(file.fd);
        }
    }
}

inline ssize_t ZeroCopyFileTransfer::sendfile(int client_fd, const std::string& file_path) {
    // 先获取文件信息
    struct stat st;
    if (stat(file_path.c_str(), &st) < 0) {
        return -1;
    }

    // 打开文件（走缓存）
    int file_fd = open_file(file_path);
    if (file_fd < 0) {
        return -1;
    }

    // 使用 sendfile 零拷贝传输
    // sendfile(out_fd, in_fd, &offset, count) - out_fd=socket, in_fd=file
    off_t offset = 0;
    ssize_t total = 0;

    while (offset < st.st_size) {
        ssize_t n = ::sendfile(client_fd, file_fd, &offset, st.st_size - offset);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }

    return total;
}

inline ssize_t ZeroCopyFileTransfer::sendfile_range(
    int client_fd,
    const std::string& file_path,
    off_t offset,
    size_t length
) {
    int file_fd = open_file(file_path);
    if (file_fd < 0) {
        return -1;
    }

    ssize_t total = 0;
    while (length > 0) {
        // sendfile(out_fd, in_fd, &offset, count) - out_fd=socket, in_fd=file
        ssize_t n = ::sendfile(client_fd, file_fd, &offset, length);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        length -= n;
        total += n;
    }

    return total;
}

inline bool ZeroCopyFileTransfer::file_exists(const std::string& path) const {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

inline /* static */ const char* ZeroCopyFileTransfer::get_mime_type(
    const std::string& path
) {
    auto ends_with = [](const std::string& s, const char* suffix) {
        size_t slen = strlen(suffix);
        return s.size() >= slen && s.compare(s.size() - slen, slen, suffix) == 0;
    };
    if (ends_with(path, ".html") || ends_with(path, ".htm")) return "text/html";
    if (ends_with(path, ".css")) return "text/css";
    if (ends_with(path, ".js")) return "application/javascript";
    if (ends_with(path, ".json")) return "application/json";
    if (ends_with(path, ".xml")) return "application/xml";
    if (ends_with(path, ".png")) return "image/png";
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) return "image/jpeg";
    if (ends_with(path, ".gif")) return "image/gif";
    if (ends_with(path, ".svg")) return "image/svg+xml";
    if (ends_with(path, ".ico")) return "image/x-icon";
    if (ends_with(path, ".pdf")) return "application/pdf";
    if (ends_with(path, ".zip")) return "application/zip";
    if (ends_with(path, ".txt")) return "text/plain";
    if (ends_with(path, ".woff")) return "font/woff";
    if (ends_with(path, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}

inline /* static */ std::string ZeroCopyFileTransfer::build_response_header(
    int status,
    const std::string& status_msg,
    size_t content_length,
    const char* mime_type,
    bool keep_alive
) {
    std::string header;
    header.reserve(512);

    header += "HTTP/1.1 ";
    header += std::to_string(status);
    header += " ";
    header += status_msg;
    header += "\r\n";

    header += "Content-Type: ";
    header += mime_type;
    header += "\r\n";

    header += "Content-Length: ";
    header += std::to_string(content_length);
    header += "\r\n";

    header += "Connection: ";
    header += keep_alive ? "keep-alive" : "close";
    header += "\r\n";

    header += "Server: HighPerfServer/1.0\r\n";
    header += "\r\n";

    return header;
}

inline double ZeroCopyFileTransfer::cache_hit_rate() const {
    int64_t total = hits_ + misses_;
    if (total == 0) return 0.0;
    return static_cast<double>(hits_) / total * 100.0;
}

inline void ZeroCopyFileTransfer::prune_expired(int64_t max_idle_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = MinHeapTimer::current_time_ms();

    auto it = cache_.begin();
    while (it != cache_.end()) {
        if (now - it->second.last_used > max_idle_ms) {
            if (it->second.fd >= 0) {
                ::close(it->second.fd);
            }
            fd_to_path_.erase(it->second.fd);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

inline int ZeroCopyFileTransfer::open_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = MinHeapTimer::current_time_ms();

    auto it = cache_.find(path);
    if (it != cache_.end()) {
        // 缓存命中
        hits_++;
        it->second.last_used = now;
        return it->second.fd;
    }

    // 缓存未命中
    misses_++;
    struct stat st;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        ::close(fd);
        return -1;
    }

    // 缓存满了，移除最旧的
    if (cache_.size() >= cache_size_) {
        int64_t oldest = now;
        std::string oldest_path;

        for (auto& [p, f] : cache_) {
            if (f.last_used < oldest) {
                oldest = f.last_used;
                oldest_path = p;
            }
        }

        if (!oldest_path.empty()) {
            auto& f = cache_[oldest_path];
            if (f.fd >= 0) {
                ::close(f.fd);
            }
            fd_to_path_.erase(f.fd);
            cache_.erase(oldest_path);
        }
    }

    cache_file(fd, path, st);
    return fd;
}

inline void ZeroCopyFileTransfer::cache_file(
    int fd,
    const std::string& path,
    const struct stat& st
) {
    CachedFile f;
    f.path = path;
    f.fd = fd;
    f.st = st;
    f.last_used = MinHeapTimer::current_time_ms();
    f.file_size = st.st_size;

    cache_[path] = std::move(f);
    fd_to_path_[fd] = path;
}

} // namespace high_perf

#endif // ZERO_COPY_HPP
