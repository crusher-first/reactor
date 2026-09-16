/**
 * @file server.cpp
 * @brief 高性能 Web 服务器主实现（基于 io_uring 的原生异步 I/O）
 */
#include "server.hpp"
#include "zero_copy.hpp"

#include <iostream>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <algorithm>
#include <limits.h>

#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

namespace high_perf {

// ---- 完成事件 user_data 编码：高 32 位为 tag，低 32 位为 fd / slot 索引 ----
static constexpr uint64_t TAG_ACCEPT = 1;
static constexpr uint64_t TAG_RECV   = 2;
static constexpr uint64_t TAG_SEND   = 3;

static inline uint64_t make_user_data(uint64_t tag, uint32_t val) {
    return (tag << 32) | static_cast<uint64_t>(val);
}

static constexpr size_t MAX_REQUEST_BYTES = 64 * 1024;
static constexpr size_t ACCEPT_SLOTS = 64;

IOURingServer::IOURingServer(const ServerConfig& config)
    : config_(config)
    , server_fd_(-1)
{
    // 忽略 SIGPIPE，避免向已关闭 socket 写入导致进程退出
    signal(SIGPIPE, SIG_IGN);
    memset(&ring_, 0, sizeof(ring_));
}

IOURingServer::~IOURingServer() {
    stop();
    cleanup();
}

bool IOURingServer::init() {
    // 创建服务器 socket
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
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
        std::cerr << "Failed to bind: " << strerror(errno) << std::endl;
        return false;
    }

    if (listen(server_fd_, config_.backlog) < 0) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        return false;
    }

    // 监听 socket 设为非阻塞（io_uring 自身会处理非阻塞 I/O）
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    // 初始化 io_uring（默认参数，非 SQPOLL）
    if (io_uring_queue_init(config_.queue_depth, &ring_, 0) < 0) {
        std::cerr << "Failed to init io_uring: " << strerror(errno) << std::endl;
        return false;
    }
    ring_ready_ = true;

    // 计算静态文件根目录的规范路径（用于路径遍历防护）
    char root_buf[PATH_MAX];
    if (realpath(config_.root_dir.c_str(), root_buf) != nullptr) {
        root_real_ = root_buf;
    }

    accept_slots_.resize(ACCEPT_SLOTS);
    accept_in_flight_.assign(ACCEPT_SLOTS, false);

    file_transfer_ = std::make_unique<ZeroCopyFileTransfer>(config_.file_cache_size);

    std::cout << "Server initialized on port " << config_.port
              << " (io_uring, queue_depth=" << config_.queue_depth << ")"
              << std::endl;
    return true;
}

void IOURingServer::start() {
    if (!init()) {
        return;
    }

    running_ = true;
    event_loop();
    running_ = false;
    cleanup();
}

void IOURingServer::stop() {
    running_ = false;
}

void IOURingServer::cleanup() {
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
    // 先关闭所有连接，再销毁 io_uring（其 CQE 可能引用连接对象）
    for (auto& kv : conns_) {
        if (kv.second && kv.second->fd >= 0) {
            ::close(kv.second->fd);
        }
    }
    conns_.clear();
    if (ring_ready_) {
        io_uring_queue_exit(&ring_);
        ring_ready_ = false;
    }
    std::cout << "Server stopped" << std::endl;
}

struct io_uring_sqe* IOURingServer::get_sqe() {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        // 提交队列已满：先提交一次，再尝试取 SQE
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
}

void IOURingServer::event_loop() {
    while (running_) {
        // 补充 accept 请求
        for (size_t i = 0; i < accept_slots_.size(); ++i) {
            if (!accept_in_flight_[i]) {
                submit_accept(i);
            }
        }

        // 提交挂起的 SQE
        io_uring_submit(&ring_);

        // 等待至少一个完成事件（1 秒超时，保证 stop() 能及时退出）
        struct io_uring_cqe* cqe = nullptr;
        struct __kernel_timespec ts{};
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);

        // 处理所有就绪的完成事件
        drain_completions();
    }
}

void IOURingServer::drain_completions() {
    unsigned head = 0;
    unsigned count = 0;
    struct io_uring_cqe* cqe = nullptr;

    io_uring_for_each_cqe(&ring_, head, cqe) {
        handle_completion(cqe);
        ++count;
    }
    io_uring_cq_advance(&ring_, count);
}

void IOURingServer::handle_completion(struct io_uring_cqe* cqe) {
    uint64_t ud = static_cast<uint64_t>(cqe->user_data);
    uint64_t tag = ud >> 32;
    uint32_t val = static_cast<uint32_t>(ud & 0xffffffffu);
    int res = cqe->res;

    switch (tag) {
    case TAG_ACCEPT: {
        accept_in_flight_[val] = false;
        if (res >= 0) {
            on_accept(res);
        }
        // 立即补充该槽位的 accept
        submit_accept(val);
        break;
    }
    case TAG_RECV: {
        auto it = conns_.find(static_cast<int>(val));
        if (it != conns_.end()) {
            on_recv(it->second.get(), res);
        }
        break;
    }
    case TAG_SEND: {
        auto it = conns_.find(static_cast<int>(val));
        if (it != conns_.end()) {
            on_send(it->second.get(), res);
        }
        break;
    }
    default:
        break;
    }
}

void IOURingServer::submit_accept(size_t slot) {
    if (slot >= accept_slots_.size()) return;

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) return;

    AcceptSlot& s = accept_slots_[slot];
    io_uring_prep_accept(sqe, server_fd_, reinterpret_cast<struct sockaddr*>(&s.addr),
                         &s.addrlen, 0);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
        static_cast<uintptr_t>(make_user_data(TAG_ACCEPT, static_cast<uint32_t>(slot)))));
    accept_in_flight_[slot] = true;
}

void IOURingServer::on_accept(int client_fd) {
    // 客户端 socket 默认独立，需显式设为非阻塞
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // 关闭 Nagle，避免头部/文件正文分两次发送时引入 ~40ms 延迟
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    auto conn = std::make_unique<HttpConn>();
    conn->fd = client_fd;
    conns_[client_fd] = std::move(conn);
    arm_recv(conns_[client_fd].get());
}

void IOURingServer::arm_recv(HttpConn* c) {
    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        close_conn(c->fd);
        return;
    }
    io_uring_prep_recv(sqe, c->fd, c->recv_buf, sizeof(c->recv_buf), 0);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
        static_cast<uintptr_t>(make_user_data(TAG_RECV, static_cast<uint32_t>(c->fd)))));
}

void IOURingServer::on_recv(HttpConn* c, int res) {
    if (res <= 0) {
        // 0 = 对端关闭；< 0 = 错误
        close_conn(c->fd);
        return;
    }
    c->in.append(c->recv_buf, static_cast<size_t>(res));
    process_input(c);
}

void IOURingServer::process_input(HttpConn* c) {
    size_t pos = c->in.find("\r\n\r\n");
    if (pos == std::string::npos) {
        if (c->in.size() > MAX_REQUEST_BYTES) {
            send_error(c, 431, "Request Header Fields Too Large");
            return;
        }
        arm_recv(c);  // 继续读取更多数据
        return;
    }

    std::string head = c->in.substr(0, pos + 4);
    c->in.erase(0, pos + 4);  // 消费当前请求，保留可能的 pipeline 数据

    if (!parse_request(c, head)) {
        send_error(c, 400, "Bad Request");
        return;
    }
    serve(c);
}

bool IOURingServer::parse_request(HttpConn* c, const std::string& head) {
    size_t line_end = head.find("\r\n");
    if (line_end == std::string::npos) return false;

    std::string line = head.substr(0, line_end);

    // 解析请求行: METHOD SP PATH SP VERSION
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    c->method = line.substr(0, sp1);
    c->path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    c->version = line.substr(sp2 + 1);

    if (c->method.empty() || c->path.empty() || c->version.empty()) return false;

    // HTTP/1.1 默认 keep-alive，HTTP/1.0 默认关闭
    c->keep_alive = (c->version == "HTTP/1.1");

    // 解析头部（大小写不敏感）
    size_t pos = line_end + 2;
    while (pos < head.size()) {
        size_t nl = head.find("\r\n", pos);
        if (nl == std::string::npos) break;
        std::string hdr = head.substr(pos, nl - pos);
        size_t colon = hdr.find(':');
        if (colon != std::string::npos) {
            std::string key = hdr.substr(0, colon);
            std::string value = hdr.substr(colon + 1);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            // 去除 value 前导空格
            size_t nsp = value.find_first_not_of(" \t");
            if (nsp != std::string::npos) value = value.substr(nsp);

            if (key == "connection") {
                std::transform(value.begin(), value.end(), value.begin(), ::tolower);
                if (value.find("close") != std::string::npos) {
                    c->keep_alive = false;
                } else if (value.find("keep-alive") != std::string::npos) {
                    c->keep_alive = true;
                }
            }
        }
        pos = nl + 2;
    }

    return true;
}

void IOURingServer::serve(HttpConn* c) {
    if (c->method != "GET") {
        send_error(c, 405, "Method Not Allowed");
        return;
    }

    std::string file_path = config_.root_dir;
    if (c->path == "/") {
        file_path += "/index.html";
    } else {
        file_path += c->path;
    }

    // 路径遍历防护：解析到的文件必须位于 root_real_ 之下
    char real_path[PATH_MAX];
    if (realpath(file_path.c_str(), real_path) == nullptr) {
        send_error(c, 404, "Not Found");
        return;
    }
    std::string resolved(real_path);
    if (!root_real_.empty() && resolved.compare(0, root_real_.size(), root_real_) != 0) {
        send_error(c, 403, "Forbidden");
        return;
    }

    struct stat st;
    if (stat(file_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        send_error(c, 404, "Not Found");
        return;
    }

    const char* mime = ZeroCopyFileTransfer::get_mime_type(file_path);
    std::string header = ZeroCopyFileTransfer::build_response_header(
        200, "OK", static_cast<size_t>(st.st_size), mime, c->keep_alive);

    // 头部先异步发送，文件正文使用零拷贝 sendfile（缓存 fd，告别人工缓冲与重复 open/read/close）
    c->out = header;
    c->out_off = 0;
    c->file_path = file_path;
    c->send_file_after = true;
    submit_send(c);
}

void IOURingServer::submit_send(HttpConn* c) {
    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        close_conn(c->fd);
        return;
    }
    const char* data = c->out.data() + c->out_off;
    size_t len = c->out.size() - c->out_off;
    io_uring_prep_send(sqe, c->fd, data, len, 0);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
        static_cast<uintptr_t>(make_user_data(TAG_SEND, static_cast<uint32_t>(c->fd)))));
}

void IOURingServer::on_send(HttpConn* c, int res) {
    if (res <= 0) {
        close_conn(c->fd);
        return;
    }

    c->out_off += static_cast<size_t>(res);
    if (c->out_off < c->out.size()) {
        submit_send(c);  // 继续发送剩余部分
        return;
    }

    // 头部已发送完毕，零拷贝发送文件正文（复用缓存 fd）
    if (c->send_file_after) {
        c->send_file_after = false;
        file_transfer_->sendfile(c->fd, c->file_path);
    }

    if (c->keep_alive) {
        process_input(c);  // 处理 pipeline 中已到达的请求，或等待下一次 recv
    } else {
        close_conn(c->fd);
    }
}

void IOURingServer::send_error(HttpConn* c, int status, const char* message) {
    std::string body = "<html><body><h1>";
    body += std::to_string(status);
    body += " ";
    body += message;
    body += "</h1></body></html>";

    std::string header = ZeroCopyFileTransfer::build_response_header(
        status, message, body.size(), "text/html", false);

    c->out = header + body;
    c->out_off = 0;
    c->keep_alive = false;
    c->send_file_after = false;
    submit_send(c);
}

void IOURingServer::close_conn(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    conns_.erase(it);
    ::close(fd);
}

} // namespace high_perf