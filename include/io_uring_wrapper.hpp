/**
 * @file io_uring_wrapper.hpp
 * @brief io_uring 异步 I/O 封装
 * @note Linux 5.10+ 内核支持，共享内存队列，原生异步，支持零拷贝
 */
#ifndef IO_URING_WRAPPER_HPP
#define IO_URING_WRAPPER_HPP

#include <liburing.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <functional>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

namespace high_perf {

/**
 * @brief io_uring 操作类型
 */
enum class IOUringOp {
    ACCEPT,
    READ,
    WRITE,
    SENDFILE,
    CLOSE,
    TIMEOUT,
    NOP
};

/**
 * @brief io_uring 事件上下文
 */
struct IOUringCQEContext {
    IOUringOp op;
    int fd;                          // 关联的文件描述符
    void* user_data;                 // 用户数据
    std::function<void(int, int)> callback; // 完成回调 (fd, res)
};

/**
 * @brief io_uring 事件封装
 */
struct IOUringEvent {
    IOUringOp op;
    int fd;
    void* buf;
    size_t len;
    off_t offset;
    void* user_data;
    std::function<void(int, int)> callback;

    IOUringEvent() = default;
    IOUringEvent(IOUringOp o, int f, void* b, size_t l, off_t off,
                 void* ud, std::function<void(int,int)> cb)
        : op(o), fd(f), buf(b), len(l), offset(off),
          user_data(ud), callback(std::move(cb)) {}
};

/**
 * @brief io_uring 封装类
 */
class IOUring {
public:
    /**
     * @param queue_depth  队列深度（SQE 数量）
     */
    explicit IOUring(int queue_depth = 256);
    ~IOUring();

    // 禁止拷贝
    IOUring(const IOUring&) = delete;
    IOUring& operator=(const IOURing&) = delete;

    /**
     * @brief 初始化是否成功
     */
    bool good() const { return good_; }

    /**
     * @brief 获取系统支持的 io_uring 特性
     */
    static uint32_t get_supported_features();

    // ---- 异步操作 ----

    /**
     * @brief 异步 accept
     */
    int accept(int server_fd, struct sockaddr* addr, socklen_t* addrlen,
               std::function<void(int, int)> callback);

    /**
     * @brief 异步 read
     */
    int read(int fd, void* buf, size_t len, off_t offset = -1,
             std::function<void(int, int)> callback = nullptr);

    /**
     * @brief 异步 write
     */
    int write(int fd, const void* buf, size_t len,
              std::function<void(int, int)> callback = nullptr);

    /**
     * @brief 异步 sendfile
     */
    int sendfile(int out_fd, int in_fd, off_t offset, size_t length,
                  std::function<void(int, int)> callback = nullptr);

    /**
     * @brief 异步 close
     */
    int close(int fd, std::function<void(int, int)> callback = nullptr);

    /**
     * @brief 提交所有挂起的 SQE
     * @return 提交的 SQE 数量
     */
    int submit();

    /**
     * @brief 批量提交
     * @return 提交的 SQE 数量
     */
    int submit_batch(const std::vector<IOUringEvent>& events);

    /**
     * @brief 处理完成事件
     * @param max_events 最大处理数量，-1 表示全部
     * @return 处理的 CQE 数量
     */
    int process_completions(int max_events = -1);

    /**
     * @brief 等待至少一个完成事件
     * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
     * @return 是否有事件
     */
    bool wait_completion(int timeout_ms = 1000);

    /**
     * @brief 获取就绪的完成事件数
     */
    int pending_count() const;

    /**
     * @brief 获取/设置队列深度
     */
    int queue_depth() const { return queue_depth_; }

private:
    struct io_uring ring_;
    int queue_depth_;
    bool good_ = false;

    std::vector<IOUringCQEContext> context_;
    std::mutex ctx_mutex_;

    // 已注册的文件描述符
    std::vector<int> registered_fds_;
    bool fd_registered_ = false;

    IOUringCQEContext* alloc_context();
    void free_context(IOUringCQEContext* ctx);
    int submit_one(struct io_uring_sqe* sqe, IOUringEvent& event);
};

// ==================== 实现 ====================

inline IOURing::IOUring(int queue_depth) : queue_depth_(queue_depth) {
    memset(&ring_, 0, sizeof(ring_));

    struct io_uring_params params = {};
    params.flags = IORING_SETUP_SQPOLL;  // 内核线程轮询（可选）
    // params.flags |= IORING_SETUP_SQPOLL; // 高性能但增加功耗

    if (io_uring_queue_init_params(queue_depth, &ring_, &params) < 0) {
        good_ = false;
        return;
    }

    good_ = true;
}

inline IOUring::~IOUring() {
    if (good_) {
        io_uring_queue_exit(&ring_);
    }
}

/* static */ inline uint32_t IOUring::get_supported_features() {
    struct io_uring_features features;
    struct io_uring_params params = {};
    params.features = &features;
    // 读取 features 需要特殊方式，这里简化处理
    return 0;
}

inline IOURingCQEContext* IOURing::alloc_context() {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    context_.push_back(IOURingCQEContext{});
    return &context_.back();
}

inline void IOURing::free_context(IOUringCQEContext* ctx) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    // 简化：上下文由 vector 管理，不显式释放
    (void)ctx;
}

inline int IOURing::accept(
    int server_fd,
    struct sockaddr* addr,
    socklen_t* addrlen,
    std::function<void(int, int)> callback
) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return -ENOMEM;

    auto* ctx = alloc_context();
    ctx->op = IOURingOp::ACCEPT;
    ctx->fd = server_fd;
    ctx->callback = std::move(callback);

    io_uring_prep_accept(sqe, server_fd, addr, addrlen, 0);
    io_uring_sqe_set_data(sqe, ctx);

    return 0;
}

inline int IOURing::read(
    int fd,
    void* buf,
    size_t len,
    off_t offset,
    std::function<void(int, int)> callback
) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return -ENOMEM;

    auto* ctx = alloc_context();
    ctx->op = IOURingOp::READ;
    ctx->fd = fd;
    ctx->callback = std::move(callback);

    if (offset >= 0) {
        io_uring_prep_read_fixed(sqe, fd, buf, len, offset, 0);
    } else {
        io_uring_prep_read(sqe, fd, buf, len, 0);
    }
    io_uring_sqe_set_data(sqe, ctx);

    return 0;
}

inline int IOURing::write(
    int fd,
    const void* buf,
    size_t len,
    std::function<void(int, int)> callback
) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return -ENOMEM;

    auto* ctx = alloc_context();
    ctx->op = IOURingOp::WRITE;
    ctx->fd = fd;
    ctx->callback = std::move(callback);

    io_uring_prep_write(sqe, fd, buf, len, 0);
    io_uring_sqe_set_data(sqe, ctx);

    return 0;
}

inline int IOURing::sendfile(
    int out_fd,
    int in_fd,
    off_t offset,
    size_t length,
    std::function<void(int, int)> callback
) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return -ENOMEM;

    auto* ctx = alloc_context();
    ctx->op = IOURingOp::SENDFILE;
    ctx->fd = out_fd;
    ctx->callback = std::move(callback);

    io_uring_prep_sendfile(sqe, out_fd, in_fd, offset, length);
    io_uring_sqe_set_data(sqe, ctx);

    return 0;
}

inline int IOURing::close(int fd, std::function<void(int, int)> callback) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return -ENOMEM;

    auto* ctx = alloc_context();
    ctx->op = IOURingOp::CLOSE;
    ctx->fd = fd;
    ctx->callback = std::move(callback);

    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data(sqe, ctx);

    return 0;
}

inline int IOURing::submit() {
    return io_uring_submit(&ring_);
}

inline int IOURing::submit_batch(const std::vector<IOUringEvent>& events) {
    int submitted = 0;
    for (const auto& ev : events) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) break;

        auto* ctx = alloc_context();
        ctx->op = ev.op;
        ctx->fd = ev.fd;
        ctx->user_data = ev.user_data;
        ctx->callback = ev.callback;

        switch (ev.op) {
            case IOUringOp::READ:
                if (ev.offset >= 0) {
                    io_uring_prep_read_fixed(sqe, ev.fd, ev.buf, ev.len, ev.offset, 0);
                } else {
                    io_uring_prep_read(sqe, ev.fd, ev.buf, ev.len, 0);
                }
                break;
            case IOURingOp::WRITE:
                io_uring_prep_write(sqe, ev.fd, ev.buf, ev.len, 0);
                break;
            case IOURingOp::SENDFILE:
                io_uring_prep_sendfile(sqe, ev.fd,
                    static_cast<int>(reinterpret_cast<uintptr_t>(ev.buf)),
                    ev.offset, ev.len);
                break;
            case IOURingOp::CLOSE:
                io_uring_prep_close(sqe, ev.fd);
                break;
            default:
                io_uring_prep_nop(sqe);
                break;
        }

        io_uring_sqe_set_data(sqe, ctx);
        submitted++;
    }

    return io_uring_submit(&ring_);
}

inline int IOURing::process_completions(int max_events) {
    struct io_uring_cqe cqe;
    int processed = 0;

    if (max_events < 0) {
        max_events = queue_depth_;
    }

    while (processed < max_events) {
        int ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) {
            if (ret == -EAGAIN) break;
            break;
        }

        auto* ctx = reinterpret_cast<IOUringCQEContext*>(cqe.user_data);
        if (ctx && ctx->callback) {
            ctx->callback(ctx->fd, cqe.res);
        }

        io_uring_cqe_seen(&ring_, cqe);
        processed++;
    }

    return processed;
}

inline bool IOURing::wait_completion(int timeout_ms) {
    struct io_ring_cqe cqe;

    if (timeout_ms < 0) {
        timeout_ms = -1;
    }

    int ret = io_uring_wait_cqe(&ring_, &cqe, timeout_ms >= 0 ? timeout_ms : nullptr);
    if (ret < 0) {
        return false;
    }

    auto* ctx = reinterpret_cast<IOUringCQEContext*>(cqe.user_data);
    if (ctx && ctx->callback) {
        ctx->callback(ctx->fd, cqe.res);
    }

    io_uring_cqe_seen(&ring_, cqe);
    return true;
}

inline int IOURing::pending_count() const {
    return io_uring_peek_cqe(const_cast<struct io_uring*>(&ring_), nullptr);
}

} // namespace high_perf

#endif // IO_URING_WRAPPER_HPP
