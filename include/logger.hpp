/**
 * @file logger.hpp
 * @brief 日志系统 - 支持多级别、文件轮转
 */
#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <memory>
#include <vector>
#include <functional>

namespace high_perf {

/**
 * @brief 日志级别
 */
enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
    FATAL = 4
};

/**
 * @brief 日志格式化器接口
 */
class LogFormatter {
public:
    virtual ~LogFormatter() = default;
    virtual std::string format(LogLevel level, const char* file, int line,
                                const std::string& message) = 0;
};

/**
 * @brief 默认格式化器
 */
class DefaultLogFormatter : public LogFormatter {
public:
    std::string format(LogLevel level, const char* file, int line,
                       const std::string& message) override {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count()
            << " [" << level_string(level) << "] "
            << "[" << file << ":" << line << "] "
            << message << '\n';
        return oss.str();
    }

private:
    static const char* level_string(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default: return "UNKNW";
        }
    }
};

/**
 * @brief 日志输出目标接口
 */
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const std::string& msg) = 0;
    virtual void flush() {}
};

/**
 * @brief 控制台输出
 */
class ConsoleSink : public LogSink {
public:
    void write(const std::string& msg) override {
        std::cout << msg;
        std::cout.flush();
    }

    void flush() override {
        std::cout.flush();
    }
};

/**
 * @brief 文件输出（支持轮转）
 */
class FileSink : public LogSink {
public:
    explicit FileSink(const std::string& path, size_t max_size = 100 * 1024 * 1024,
                      size_t max_files = 10)
        : file_path_(path)
        , max_size_(max_size)
        , max_files_(max_files)
    {
        rotate();
    }

    void write(const std::string& msg) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ofs_ || !ofs_->is_open()) return;

        // 检查是否需要轮转
        if (ofs_->tellp() >= static_cast<std::streampos>(max_size_)) {
            ofs_->close();
            rotate();
        }

        if (ofs_ && ofs_->is_open()) {
            *ofs_ << msg;
        }
    }

    void flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ofs_) ofs_->flush();
    }

private:
    void rotate() {
        // 关闭旧文件
        ofs_.reset();

        // 打开新文件
        ofs_ = std::make_unique<std::ofstream>(
            file_path_, std::ios::app | std::ios::binary);

        if (!ofs_->is_open()) {
            std::cerr << "Failed to open log file: " << file_path_ << std::endl;
        }
    }

    std::string file_path_;
    size_t max_size_;
    size_t max_files_;
    std::unique_ptr<std::ofstream> ofs_;
    std::mutex mutex_;
};

/**
 * @brief 异步日志器
 */
class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void init(LogLevel level, std::unique_ptr<LogSink> sink = nullptr) {
        level_ = level;
        if (sink) {
            sink_ = std::move(sink);
        }
    }

    template<typename... Args>
    void log(LogLevel level, const char* file, int line,
             const char* fmt, Args&&... args) {
        if (level < level_.load()) return;

        // 格式化消息
        char buffer[4096];
        snprintf(buffer, sizeof(buffer), fmt, std::forward<Args>(args)...);

        std::string msg = formatter_.format(level, file, line, buffer);

        // 写入
        if (sink_) {
            sink_->write(msg);
        }
    }

    void set_level(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_.load(); }

    // 便捷宏
#define LOG_DEBUG(fmt, ...) \
    log(LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    log(LogLevel::INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
    log(LogLevel::WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    log(LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) \
    log(LogLevel::FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

private:
    Logger() {
        // 默认控制台输出
        sink_ = std::make_unique<ConsoleSink>();
        level_ = LogLevel::INFO;
    }

    std::atomic<LogLevel> level_;
    std::unique_ptr<LogSink> sink_;
    DefaultLogFormatter formatter_;
};

// 简化使用
#define LoggerInit(level) \
    high_perf::Logger::instance().init(level)

} // namespace high_perf

#endif // LOGGER_HPP
