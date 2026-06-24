/**
 * @file server.cpp
 * @brief 高性能 Web 服务器主实现
 */
#include "server.hpp"
#include "io_uring_wrapper.hpp"
#include "connection.hpp"
#include "zero_copy.hpp"
#include <iostream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace high_perf {

IOURingServer::IOURingServer(const ServerConfig& config)
    : config_(config)
    , running_(false)
    , server_fd_(-1)
    , stop_eventfd_(-1)
{
    // 忽略 SIGPIPE
    signal(SIGPIPE, SIG_IGN);
}

IOURingServer::~IOURingServer() {
    stop();
}

bool IOURingServer::init() {
    // 创建 io_uring 实例
    ring_ = std::make_unique<IOURing>(config_.queue_depth);
    if (!ring_->good()) {
        std::cerr << "Failed to initialize io_uring" << std::endl;
        return false;
    }

    // 创建服务器 socket
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }

    // 设置 SO_REUSEADDR
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind: " << strerror(errno) << std::endl;
        return false;
    }

    // 监听
    if (listen(server_fd_, config_.backlog) < 0) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        return false;
    }

    // 设置为非阻塞
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    // 创建连接管理器
    conn_manager_ = std::make_unique<ConnectionManager>();
    conn_manager_->set_close_callback([this](int fd) {
        ring_->close(fd, nullptr);
    });

    // 创建文件传输器
    file_transfer_ = std::make_unique<ZeroCopyFileTransfer>(config_.file_cache_size);

    // 创建线程池
    thread_pool_ = std::make_unique<ThreadPool>(config_.worker_threads);

    // 创建定时器
    timer_ = std::make_unique<MinHeapTimer>();

    std::cout << "Server initialized on port " << config_.port << std::endl;
    return true;
}

void IOURingServer::start() {
    if (!init()) {
        return;
    }

    running_ = true;

    // 注册 accept
    accept_connections();

    // 启动事件循环
    event_loop();

    running_ = false;
    cleanup();
}

void IOURingServer::stop() {
    running_ = false;
}

void IOURingServer::event_loop() {
    while (running_) {
        // 处理定时器
        timer_->tick();

        // 处理 io_uring 完成事件
        int processed = ring_->process_completions(64);

        // 如果没有事件，短暂等待
        if (processed == 0) {
            usleep(1000); // 1ms
        }

        // 定期提交 accept
        accept_connections();
    }
}

void IOURingServer::accept_connections() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = ::accept(server_fd_, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "Accept error: " << strerror(errno) << std::endl;
            }
            return;
        }

        // 设置为非阻塞
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        // 创建连接
        auto conn = conn_manager_->create_connection(client_fd);

        // 更新活跃时间
        conn_manager_->touch_connection(conn);

        // 处理请求
        handle_request(conn);
    }
}

void IOURingServer::handle_request(std::shared_ptr<ConnectionContext> conn) {
    // 提交异步读
    Buffer* buf = conn->read_buffer.get();
    if (!buf) return;

    buf->reserve(config_.buffer_size);

    ssize_t n = ::read(conn->fd, buf->data(), buf->capacity() - 1);
    if (n > 0) {
        buf->resize(static_cast<size_t>(n));
        // 确保 buffer 以 null 结尾，供 strstr/sscanf 等字符串函数使用
        buf->data()[n] = '\0';
        conn_manager_->touch_connection(conn);

        // 解析请求
        if (parse_request(conn)) {
            // 处理请求
            process_request(conn);
        }
    } else if (n == 0) {
        // 客户端关闭连接
        conn_manager_->close_connection(conn);
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            conn_manager_->close_connection(conn);
        }
    }
}

bool IOURingServer::parse_request(std::shared_ptr<ConnectionContext> conn) {
    Buffer* buf = conn->read_buffer.get();
    if (!buf || buf->size() == 0) return false;

    // 简单的 HTTP 请求解析
    const char* data = buf->data();

    // 查找请求行结束
    const char* end = strstr(data, "\r\n");
    if (!end) return false;

    // 解析请求行: GET /path HTTP/1.1
    int n = sscanf(data, "%255s %255s %255s",
                   request_line_.method,
                   request_line_.path,
                   request_line_.version);
    if (n != 3) return false;

    // 解析头部
    const char* header_start = end + 2;
    const char* header_end = strstr(header_start, "\r\n\r\n");
    if (!header_end) return false;

    conn->request.clear();
    conn->request.method = request_line_.method;
    conn->request.path = request_line_.path;
    conn->request.version = request_line_.version;

    // 简单解析头部
    const char* p = header_start;
    while (p < header_end) {
        const char* colon = strchr(p, ':');
        const char* line_end = strstr(p, "\r\n");
        if (colon && line_end && colon < line_end) {
            std::string key(p, colon - p);
            // HTTP header 名大小写不敏感，统一转小写
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            // 跳过 ": "
            std::string value(colon + 2, line_end - colon - 2);
            conn->request.headers[key] = value;
        }
        p = line_end + 2;
    }

    return true;
}

void IOURingServer::process_request(std::shared_ptr<ConnectionContext> conn) {
    const std::string& path = conn->request.path;
    const std::string& method = conn->request.method;

    // 当前事件循环不支持对 keep-alive 连接的后续请求重新读数据，
    // 因此强制关闭连接以避免客户端挂起等待
    conn->keep_alive = false;

    // 处理静态文件
    if (method == "GET") {
        std::string file_path = config_.root_dir;
        if (path == "/") {
            file_path += "/index.html";
        } else {
            file_path += path;
        }

        // 安全检查：规范化路径防止路径遍历攻击
        char real_path[PATH_MAX];
        if (realpath(file_path.c_str(), real_path) == nullptr) {
            // 文件不存在返回 404，其他错误返回 400
            if (errno == ENOENT || errno == ENOTDIR) {
                send_error(conn, 404, "Not Found");
            } else {
                send_error(conn, 400, "Bad Request");
            }
            return;
        }
        std::string resolved_path(real_path);
        char root_buf[PATH_MAX];
        if (realpath(config_.root_dir.c_str(), root_buf) == nullptr) {
            send_error(conn, 500, "Internal Error");
            return;
        }
        std::string root_real(root_buf);
        if (resolved_path.compare(0, root_real.size(), root_real) != 0) {
            send_error(conn, 403, "Forbidden");
            return;
        }

        struct stat st;
        if (stat(file_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            // 发送文件
            const char* mime = ZeroCopyFileTransfer::get_mime_type(file_path);
            std::string header = ZeroCopyFileTransfer::build_response_header(
                200, "OK", st.st_size, mime, conn->keep_alive);

            // 发送头部
            ssize_t h = ::write(conn->fd, header.data(), header.size());
            if (h > 0) {
                // 发送文件内容（零拷贝）
                ssize_t sent = file_transfer_->sendfile(conn->fd, file_path);
                (void)sent;
            }

            if (!conn->keep_alive) {
                conn_manager_->close_connection(conn);
            } else {
                conn->read_buffer->clear();
                conn_manager_->touch_connection(conn);
            }
            return;
        }
    }

    // 404
    send_error(conn, 404, "Not Found");
}

void IOURingServer::send_error(std::shared_ptr<ConnectionContext> conn,
                                int status, const char* message) {
    std::string body = "<html><body><h1>";
    body += std::to_string(status);
    body += " ";
    body += message;
    body += "</h1></body></html>";

    std::string header = ZeroCopyFileTransfer::build_response_header(
        status, message, body.size(), "text/html", false);

    ssize_t hw = ::write(conn->fd, header.data(), header.size());
    if (hw > 0) {
        ssize_t bw = ::write(conn->fd, body.data(), body.size());
        (void)bw;
    }

    conn_manager_->close_connection(conn);
}

void IOURingServer::cleanup() {
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
    conn_manager_.reset();
    ring_.reset();
    std::cout << "Server stopped" << std::endl;
}

} // namespace high_perf
