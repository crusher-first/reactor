/**
 * @file main.cpp
 * @brief 高性能 C++ Web 服务器 - 程序入口
 */
#include "server.hpp"
#include <iostream>
#include <csignal>
#include <getopt.h>

using namespace high_perf;

// 全局服务器实例（用于信号处理）
static std::unique_ptr<IOURingServer> g_server;

static void signal_handler(int sig) {
    (void)sig;
    if (g_server) {
        g_server->stop();
    }
}

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -p, --port <port>        Port to listen (default: 8080)\n"
              << "  -t, --threads <num>      Worker threads (default: 4)\n"
              << "  -q, --queue-depth <num> io_uring queue depth (default: 256)\n"
              << "  -r, --root <dir>        Static files root directory (default: ./www)\n"
              << "  -c, --cache-size <num>  File cache size (default: 100)\n"
              << "  -b, --backlog <num>      Listen backlog (default: 512)\n"
              << "  -h, --help               Show this help\n"
              << "\n"
              << "Example:\n"
              << "  " << prog << " -p 8080 -t 4 -r /var/www\n";
}

int main(int argc, char* argv[]) {
    ServerConfig config;

    // 命令行参数解析
    static struct option long_options[] = {
        {"port",        required_argument, 0, 'p'},
        {"threads",     required_argument, 0, 't'},
        {"queue-depth", required_argument, 0, 'q'},
        {"root",        required_argument, 0, 'r'},
        {"cache-size",  required_argument, 0, 'c'},
        {"backlog",     required_argument, 0, 'b'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "p:t:q:r:c:b:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                config.port = std::stoi(optarg);
                break;
            case 't':
                config.worker_threads = std::stoi(optarg);
                break;
            case 'q':
                config.queue_depth = std::stoi(optarg);
                break;
            case 'r':
                config.root_dir = optarg;
                break;
            case 'c':
                config.file_cache_size = std::stoul(optarg);
                break;
            case 'b':
                config.backlog = std::stoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    std::cout << "========================================\n"
              << "  High-Performance C++ Web Server\n"
              << "========================================\n"
              << "  Port:         " << config.port << "\n"
              << "  Threads:      " << config.worker_threads << "\n"
              << "  Queue Depth:  " << config.queue_depth << "\n"
              << "  Root Dir:     " << config.root_dir << "\n"
              << "  Cache Size:   " << config.file_cache_size << "\n"
              << "========================================\n";

    // 创建服务器
    g_server = std::make_unique<IOURingServer>(config);

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 启动服务器
    try {
        g_server->start();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Server exited normally" << std::endl;
    return 0;
}
