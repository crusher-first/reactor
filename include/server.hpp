/**
 * @file server.hpp
 * @brief 高性能 Web 服务器头文件
 */
#ifndef SERVER_HPP
#define SERVER_HPP

#include "io_uring_wrapper.hpp"
#include "connection.hpp"
#include "zero_copy.hpp"
#include "timer.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>

namespace high_perf {

/**
 * @brief 服务器配置
 */
struct ServerConfig {
    int port = 8080;              // 监听端口
    int queue_depth = 256;         // io_uring 队列深度
    int worker_threads = 4;        // 工作线程数
    int backlog = 512;             // listen backlog
    size_t file_cache_size = 100; // 文件缓存数量
    int buffer_size = 8192;        // 缓冲区大小
    int64_t connection_timeout_ms = 30000; // 连接超时（毫秒）
    std::string root_dir = "./www"; // 静态文件根目录
};

/**
 * @brief 线程池
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();

    /**
     * @brief 提交任务
     */
    template<typename F>
    void submit(F&& task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::forward<F>(task));
        }
        cond_.notify_one();
    }

    /**
     * @brief 停止线程池
     */
    void stop();

    /**
     * @brief 等待所有任务完成
     */
    void wait_all();

    size_t size() const { return threads_.size(); }

private:
    void worker_loop();

    std::vector<std::thread> threads_;
    std::vector<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> stop_{false};
    std::atomic<size_t> active_tasks_{0};
};

/**
 * @brief 高性能 Web 服务器
 */
class IOURingServer {
public:
    explicit IOURingServer(const ServerConfig& config);
    ~IOURingServer();

    /**
     * @brief 启动服务器
     */
    void start();

    /**
     * @brief 停止服务器
     */
    void stop();

    /**
     * @brief 是否正在运行
     */
    bool running() const { return running_.load(); }

    /**
     * @brief 获取配置
     */
    const ServerConfig& config() const { return config_; }

private:
    bool init();
    void event_loop();
    void accept_connections();
    void handle_request(std::shared_ptr<ConnectionContext> conn);
    bool parse_request(std::shared_ptr<ConnectionContext> conn);
    void process_request(std::shared_ptr<ConnectionContext> conn);
    void send_error(std::shared_ptr<ConnectionContext> conn, int status, const char* message);
    void cleanup();

    // 临时请求行解析
    struct {
        char method[256];
        char path[256];
        char version[256];
    } request_line_;

    ServerConfig config_;
    std::atomic<bool> running_;

    int server_fd_;
    int stop_eventfd_;

    std::unique_ptr<IOURing> ring_;
    std::unique_ptr<ConnectionManager> conn_manager_;
    std::unique_ptr<ZeroCopyFileTransfer> file_transfer_;
    std::unique_ptr<ThreadPool> thread_pool_;
    std::unique_ptr<MinHeapTimer> timer_;
};

// ==================== ThreadPool 实现 ====================

inline ThreadPool::ThreadPool(size_t threads) {
    for (size_t i = 0; i < threads; ++i) {
        threads_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

inline ThreadPool::~ThreadPool() {
    stop();
}

inline void ThreadPool::worker_loop() {
    while (!stop_.load()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] {
                return stop_.load() || !tasks_.empty();
            });

            if (stop_.load() && tasks_.empty()) {
                return;
            }

            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.erase(tasks_.begin());
            }
        }

        if (task) {
            active_tasks_++;
            task();
            active_tasks_--;
        }
    }
}

inline void ThreadPool::stop() {
    stop_.store(true);
    cond_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

inline void ThreadPool::wait_all() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (tasks_.empty() && active_tasks_.load() == 0) {
                break;
            }
        }
    }
}

} // namespace high_perf

#endif // SERVER_HPP
