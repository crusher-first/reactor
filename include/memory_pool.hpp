/**
 * @file memory_pool.hpp
 * @brief 内存池分配器 - 减少 malloc/free 开销，提高缓存局部性
 */
#ifndef MEMORY_POOL_HPP
#define MEMORY_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <vector>
#include <list>
#include <mutex>
#include <memory>

namespace high_perf {

/**
 * @brief 内存块描述符
 */
struct MemoryBlock {
    void* ptr = nullptr;              // 内存块指针
    size_t size = 0;                  // 块大小
    bool in_use = false;              // 是否已被使用

    MemoryBlock() = default;
    MemoryBlock(void* p, size_t s) : ptr(p), size(s), in_use(true) {}
};

/**
 * @brief 通用内存池分配器
 * @tparam T 元素类型
 * @note 使用空闲链表管理，支持 O(1) 分配和释放
 */
template<typename T>
class MemoryPool {
public:
    /**
     * @brief 构造函数
     * @param block_size  每个内存块包含的元素数量
     * @param initial_blocks 初始预分配的块数
     */
    explicit MemoryPool(size_t block_size = 128, size_t initial_blocks = 4);

    ~MemoryPool();

    // 禁止拷贝
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    /**
     * @brief 从内存池分配一个对象
     * @return 分配的对象指针
     */
    T* allocate();

    /**
     * @brief 释放一个对象回内存池
     * @param ptr 要释放的对象指针
     */
    void deallocate(T* ptr);

    /**
     * @brief 预分配内存块
     * @param count 要分配的块数
     */
    void preallocate(size_t count);

    /**
     * @brief 获取空闲对象数量
     */
    size_t free_count() const;

    /**
     * @brief 获取总对象数量
     */
    size_t total_count() const;

private:
    size_t block_size_;               // 每个块的元素数量
    std::vector<T*> blocks_;          // 所有分配的内存块
    std::list<T*> free_list_;         // 空闲对象链表
    mutable std::mutex mutex_;         // 线程安全
};

/**
 * @brief 缓冲区内存池（专门用于 Buffer）
 */
class BufferPool {
public:
    explicit BufferPool(size_t buffer_size = 4096, size_t initial_buffers = 64);
    ~BufferPool();

    void* allocate();
    void deallocate(void* ptr);

    size_t buffer_size() const { return buffer_size_; }

private:
    size_t buffer_size_;
    std::vector<void*> blocks_;
    std::list<void*> free_list_;
    std::mutex mutex_;
};

/**
 * @brief 连接内存池
 */
struct Connection {
    int fd = -1;                      // 文件描述符
    void* read_buffer = nullptr;      // 读缓冲区
    void* write_buffer = nullptr;     // 写缓冲区
    uint32_t timer_id = 0;            // 定时器ID
    int64_t last_active_time = 0;    // 最后活动时间 (ms)
    bool keep_alive = true;           // 是否保持连接
};

class ConnectionPool {
public:
    explicit ConnectionPool(size_t max_connections = 1024);
    ~ConnectionPool();

    Connection* acquire();
    void release(Connection* conn);
    size_t active_count() const;
    size_t total_count() const;

private:
    std::vector<Connection> connections_;
    std::list<Connection*> free_list_;
    mutable std::mutex mutex_;
    size_t max_connections_;
};

// ==================== 实现 ====================

template<typename T>
MemoryPool<T>::MemoryPool(size_t block_size, size_t initial_blocks)
    : block_size_(block_size) {
    preallocate(initial_blocks);
}

template<typename T>
MemoryPool<T>::~MemoryPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (T* block : blocks_) {
        ::operator delete(block);
    }
}

template<typename T>
T* MemoryPool<T>::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!free_list_.empty()) {
        T* ptr = free_list_.front();
        free_list_.pop_front();
        return ptr;
    }

    // 需要分配新的块
    T* block = static_cast<T*>(::operator new(block_size_ * sizeof(T)));
    blocks_.push_back(block);

    // 第一个对象返回，其余加入空闲链表
    T* first = block;
    for (size_t i = 1; i < block_size_; ++i) {
        free_list_.push_back(block + i);
    }
    return first;
}

template<typename T>
void MemoryPool<T>::deallocate(T* ptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    free_list_.push_back(ptr);
}

template<typename T>
void MemoryPool<T>::preallocate(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t b = 0; b < count; ++b) {
        T* block = static_cast<T*>(::operator new(block_size_ * sizeof(T)));
        blocks_.push_back(block);

        for (size_t i = 0; i < block_size_; ++i) {
            free_list_.push_back(block + i);
        }
    }
}

template<typename T>
size_t MemoryPool<T>::free_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_list_.size();
}

template<typename T>
size_t MemoryPool<T>::total_count() const {
    return blocks_.size() * block_size_;
}

} // namespace high_perf

#endif // MEMORY_POOL_HPP
