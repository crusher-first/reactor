/**
 * @file server.hpp
 * @brief 高性能 Web 服务器头文件（基于 io_uring）
 */
#ifndef SERVER_HPP
#define SERVER_HPP

#include <liburing.h>
#include "zero_copy.hpp"

#include <sys/socket.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace high_perf {

/**
 * @brief 服务器配置
 */
struct ServerConfig {
    int port = 8080;              // 监听端口
    int queue_depth = 256;         // io_uring 队列深度
    int worker_threads = 4;        // 工作线程数（当前 io_uring 实现为单线程事件循环）
    int backlog = 512;             // listen backlog
    size_t file_cache_size = 100; // 文件缓存数量
    int buffer_size = 8192;        // 缓冲区大小
    int64_t connection_timeout_ms = 30000; // 连接超时（毫秒）
    std::string root_dir = "./www"; // 静态文件根目录
};

/**
 * @brief 高性能 Web 服务器（基于 io_uring 的原生异步 I/O）
 */
class IOURingServer {
public:
    explicit IOURingServer(const ServerConfig& config);
    ~IOURingServer();

    void start();
    void stop();
    bool running() const { return running_.load(); }
    const ServerConfig& config() const { return config_; }

private:
    struct HttpConn {
        int fd = -1;
        std::string in;              // 未处理完毕的请求字节（可能含 pipeline）
        std::string out;             // 待发送的响应头部
        size_t out_off = 0;          // 已发送偏移
        bool keep_alive = true;
        std::string method, path, version;
        std::string file_path;       // 头部发送完毕后需零拷贝发送的文件
        bool send_file_after = false;
        char recv_buf[4096];
    };

    struct AcceptSlot {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
    };

    bool init();
    void cleanup();

    void event_loop();
    void drain_completions();
    void handle_completion(struct io_uring_cqe* cqe);

    struct io_uring_sqe* get_sqe();

    void submit_accept(size_t slot);
    void on_accept(int client_fd);

    void arm_recv(HttpConn* c);
    void on_recv(HttpConn* c, int res);
    void process_input(HttpConn* c);

    void submit_send(HttpConn* c);
    void on_send(HttpConn* c, int res);

    bool parse_request(HttpConn* c, const std::string& head);
    void serve(HttpConn* c);
    void send_error(HttpConn* c, int status, const char* message);
    void close_conn(int fd);

    ServerConfig config_;
    std::atomic<bool> running_{false};

    int server_fd_ = -1;
    struct io_uring ring_;
    bool ring_ready_ = false;

    std::vector<AcceptSlot> accept_slots_;
    std::vector<bool> accept_in_flight_;
    std::unordered_map<int, std::unique_ptr<HttpConn>> conns_;

    std::unique_ptr<ZeroCopyFileTransfer> file_transfer_;

    std::string root_real_;
};

} // namespace high_perf

#endif // SERVER_HPP