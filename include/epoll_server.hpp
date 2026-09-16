/**
 * @file epoll_server.hpp
 * @brief 基于 epoll 的传统事件驱动 Web 服务器（对照组）
 */
#ifndef EPOLL_SERVER_HPP
#define EPOLL_SERVER_HPP

#include "server.hpp"
#include "connection.hpp"
#include <sys/epoll.h>
#include <cstdint>
#include <unordered_map>

namespace high_perf {

/**
 * @brief 基于 epoll 的 Web 服务器（对照组）
 * @note 与 IOURingServer 复用相同的 HTTP 解析、零拷贝发送与连接管理逻辑，
 *       唯一区别是 I/O 多路复用机制：本类使用 epoll_wait 而非 io_uring。
 */
class EpollServer {
public:
    explicit EpollServer(const ServerConfig& config);
    ~EpollServer();

    /**
     * @brief 启动服务器（阻塞直到 stop() 被调用）
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

private:
    bool init();
    void event_loop();
    void accept_connections();
    void handle_read(std::shared_ptr<ConnectionContext> conn);
    bool parse_request(std::shared_ptr<ConnectionContext> conn);
    void process_request(std::shared_ptr<ConnectionContext> conn);
    void send_error(std::shared_ptr<ConnectionContext> conn, int status, const char* message);
    void add_fd(int fd, uint32_t events);
    void del_fd(int fd);

    // 临时请求行解析
    struct {
        char method[256];
        char path[256];
        char version[256];
    } request_line_;

    ServerConfig config_;
    std::atomic<bool> running_;

    int server_fd_;
    int epoll_fd_;

    std::unique_ptr<ConnectionManager> conn_manager_;
    std::unique_ptr<ZeroCopyFileTransfer> file_transfer_;
    std::unordered_map<int, std::shared_ptr<ConnectionContext>> fd_map_;
};

} // namespace high_perf

#endif // EPOLL_SERVER_HPP