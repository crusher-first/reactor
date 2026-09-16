/**
 * @file epoll_server.cpp
 * @brief 基于 epoll 的 Web 服务器实现（对照组）
 */
#include "epoll_server.hpp"

#include <iostream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <vector>
#include <limits.h>
#include <signal.h>
#include <cerrno>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

namespace high_perf {

EpollServer::EpollServer(const ServerConfig& config)
    : config_(config)
    , running_(false)
    , server_fd_(-1)
    , epoll_fd_(-1)
{
    // 忽略 SIGPIPE，避免向已关闭 socket 写入导致进程退出
    signal(SIGPIPE, SIG_IGN);
}

EpollServer::~EpollServer() {
    stop();
}

bool EpollServer::init() {
    // 创建服务器 socket
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[epoll] Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[epoll] Failed to bind: " << strerror(errno) << std::endl;
        return false;
    }

    if (listen(server_fd_, config_.backlog) < 0) {
        std::cerr << "[epoll] Failed to listen: " << strerror(errno) << std::endl;
        return false;
    }

    // 设置为非阻塞
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    // 创建 epoll 实例
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        std::cerr << "[epoll] Failed to create epoll: " << strerror(errno) << std::endl;
        return false;
    }

    // 连接管理器：关闭连接时同步移除 epoll 监听与 fd 索引
    conn_manager_ = std::make_unique<ConnectionManager>();
    conn_manager_->set_close_callback([this](int fd) {
        del_fd(fd);
        fd_map_.erase(fd);
    });

    // 文件传输器
    file_transfer_ = std::make_unique<ZeroCopyFileTransfer>(config_.file_cache_size);

    add_fd(server_fd_, EPOLLIN);

    std::cout << "[epoll] Server initialized on port " << config_.port << std::endl;
    return true;
}

void EpollServer::start() {
    if (!init()) {
        return;
    }

    running_ = true;
    event_loop();
    running_ = false;

    // 清理：先移除 fd 索引，再释放连接管理器（其析构会触发 close_callback）
    fd_map_.clear();
    conn_manager_.reset();
    file_transfer_.reset();

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

void EpollServer::stop() {
    running_ = false;
}

void EpollServer::event_loop() {
    const int MAX_EVENTS = 1024;
    std::vector<struct epoll_event> events(MAX_EVENTS);

    while (running_) {
        // 1 秒超时，保证 stop() 能及时退出循环
        int nfds = ::epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[epoll] epoll_wait error: " << strerror(errno) << std::endl;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (fd == server_fd_) {
                accept_connections();
                continue;
            }

            auto it = fd_map_.find(fd);
            if (it != fd_map_.end()) {
                handle_read(it->second);
            }
        }
    }
}

void EpollServer::accept_connections() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = ::accept(server_fd_, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[epoll] Accept error: " << strerror(errno) << std::endl;
            }
            return;
        }

        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        auto conn = conn_manager_->create_connection(client_fd);
        conn_manager_->touch_connection(conn);

        fd_map_[client_fd] = conn;
        add_fd(client_fd, EPOLLIN);
    }
}

void EpollServer::handle_read(std::shared_ptr<ConnectionContext> conn) {
    if (!conn) return;
    Buffer* buf = conn->read_buffer.get();
    if (!buf) return;

    // 持续读取直到 EAGAIN / EOF
    while (true) {
        if (buf->size() + 1024 > buf->capacity()) {
            buf->reserve(buf->capacity() * 2 + 1024);
        }

        ssize_t n = ::read(conn->fd, buf->data() + buf->size(),
                           buf->capacity() - buf->size() - 1);
        if (n > 0) {
            buf->resize(buf->size() + n);
            conn_manager_->touch_connection(conn);
            continue;
        }
        if (n == 0) {
            conn_manager_->close_connection(conn);
            return;
        }
        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        conn_manager_->close_connection(conn);
        return;
    }

    if (buf->size() == 0) return;

    // 保证空余 1 字节可供 strstr/sscanf 安全读取（capacity - size - 1 >= 0）
    buf->data()[buf->size()] = '\0';

    if (parse_request(conn)) {
        process_request(conn);
        return;
    }

    // 解析失败：若已收到完整头部则是坏请求，否则等待更多数据
    if (strstr(buf->data(), "\r\n\r\n") != nullptr) {
        send_error(conn, 400, "Bad Request");
    }
}

bool EpollServer::parse_request(std::shared_ptr<ConnectionContext> conn) {
    Buffer* buf = conn->read_buffer.get();
    if (!buf || buf->size() == 0) return false;

    const char* data = buf->data();

    const char* end = strstr(data, "\r\n");
    if (!end) return false;

    int n = sscanf(data, "%255s %255s %255s",
                   request_line_.method,
                   request_line_.path,
                   request_line_.version);
    if (n != 3) return false;

    const char* header_start = end + 2;
    const char* header_end = strstr(header_start, "\r\n\r\n");
    if (!header_end) return false;

    conn->request.clear();
    conn->request.method = request_line_.method;
    conn->request.path = request_line_.path;
    conn->request.version = request_line_.version;

    const char* p = header_start;
    while (p < header_end) {
        const char* colon = strchr(p, ':');
        const char* line_end = strstr(p, "\r\n");
        if (colon && line_end && colon < line_end) {
            std::string key(p, colon - p);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            std::string value(colon + 2, line_end - colon - 2);
            conn->request.headers[key] = value;
        }
        p = line_end + 2;
    }

    return true;
}

void EpollServer::process_request(std::shared_ptr<ConnectionContext> conn) {
    const std::string& path = conn->request.path;
    const std::string& method = conn->request.method;

    // 与 IOURingServer 保持一致：单请求单响应，处理完即关闭
    conn->keep_alive = false;

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
            const char* mime = ZeroCopyFileTransfer::get_mime_type(file_path);
            std::string header = ZeroCopyFileTransfer::build_response_header(
                200, "OK", st.st_size, mime, conn->keep_alive);

            ssize_t h = ::write(conn->fd, header.data(), header.size());
            if (h > 0) {
                // 零拷贝发送文件内容
                ssize_t sent = file_transfer_->sendfile(conn->fd, file_path);
                (void)sent;
            }

            conn_manager_->close_connection(conn);
            return;
        }
    }

    send_error(conn, 404, "Not Found");
}

void EpollServer::send_error(std::shared_ptr<ConnectionContext> conn,
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

void EpollServer::add_fd(int fd, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        std::cerr << "[epoll] epoll_ctl ADD error: " << strerror(errno) << std::endl;
    }
}

void EpollServer::del_fd(int fd) {
    if (epoll_fd_ < 0) return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev);
}

} // namespace high_perf