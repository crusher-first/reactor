/**
 * @file timer.hpp
 * @brief 定时器系统 - 最小堆定时器 + 时间轮定时器
 */
#ifndef TIMER_HPP
#define TIMER_HPP

#include <cstdint>
#include <functional>
#include <vector>
#include <queue>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <cassert>

namespace high_perf {

/**
 * @brief 定时器回调函数类型
 */
using TimerCallback = std::function<void()>;

/**
 * @brief 定时器节点（最小堆用）
 */
struct TimerNode {
    uint64_t id;              // 唯一标识
    int64_t expire;           // 到期时间 (ms)
    int64_t interval;         // 重复间隔 (ms)，0 表示一次性
    TimerCallback callback;   // 回调函数

    TimerNode() = default;
    TimerNode(uint64_t i, int64_t e, int64_t inv, TimerCallback cb)
        : id(i), expire(e), interval(inv), callback(std::move(cb)) {}

    bool operator<(const TimerNode& other) const {
        return expire > other.expire; // 最小堆：小的在顶部
    }
};

/**
 * @brief 最小堆定时器
 * @note 添加/删除: O(log N)，执行到期: O(k log N)
 */
class MinHeapTimer {
public:
    MinHeapTimer();
    ~MinHeapTimer();

    /**
     * @brief 添加一次性定时器
     * @param timeout_ms  超时时间（毫秒）
     * @param callback    回调函数
     * @return 定时器ID
     */
    uint64_t add_timer(int64_t timeout_ms, TimerCallback callback);

    /**
     * @brief 添加重复定时器
     * @param interval_ms 重复间隔（毫秒）
     * @param callback    回调函数
     * @return 定时器ID
     */
    uint64_t add_timer_repeat(int64_t interval_ms, TimerCallback callback);

    /**
     * @brief 取消定时器
     * @param id 定时器ID
     */
    void remove_timer(uint64_t id);

    /**
     * @brief 处理到期的定时器
     * @return 下一个定时器到期的时间（毫秒），-1 表示没有定时器
     */
    int64_t tick();

    /**
     * @brief 获取当前时间（毫秒）
     */
    static int64_t current_time_ms();

    /**
     * @brief 定时器数量
     */
    size_t size() const;

private:
    void heapify_up(size_t index);
    void heapify_down(size_t index);
    void del(size_t index);

    std::vector<TimerNode> heap_;                  // 堆存储
    std::unordered_map<uint64_t, size_t> id_map_; // ID -> 索引映射
    uint64_t next_id_ = 1;                         // 下一个ID
    mutable std::mutex mutex_;
};

/**
 * @brief 时间轮定时器（环形缓冲区实现）
 * @note 添加/删除/ tick: 均为 O(1)
 */
class TimingWheel {
public:
    /**
     * @param tick_ms     每格时间（毫秒）
     * @param wheel_size  轮子格子数
     * @param max_timeout 最大超时时间
     */
    TimingWheel(int64_t tick_ms, size_t wheel_size, int64_t max_timeout);

    /**
     * @brief 添加定时器
     * @param timeout_ms  超时时间（毫秒）
     * @param callback    回调函数
     * @return 定时器ID
     */
    uint64_t add_timer(int64_t timeout_ms, TimerCallback callback);

    /**
     * @brief 删除定时器
     * @param id 定时器ID
     */
    void remove_timer(uint64_t id);

    /**
     * @brief 时间前进一格（驱动函数）
     * @return 到期回调数量
     */
    size_t tick();

    /**
     * @brief 推进到指定时间
     * @param now 当前时间
     */
    void advance(int64_t now);

    /**
     * @brief 获取距下一次 tick 的时间
     */
    int64_t next_timeout() const;

private:
    struct WheelNode {
        uint64_t id;
        TimerCallback callback;
        int64_t expire;
        WheelNode* next;

        WheelNode(uint64_t i, TimerCallback cb, int64_t e)
            : id(i), callback(std::move(cb)), expire(e), next(nullptr) {}
    };

    int64_t tick_ms_;
    size_t wheel_size_;
    int64_t max_timeout_;
    int64_t current_time_;

    std::vector<WheelNode*> slots_;               // 时间轮槽
    std::unordered_map<uint64_t, WheelNode*> node_map_;
    uint64_t next_id_ = 1;

    mutable std::mutex mutex_;
};

// ==================== MinHeapTimer 实现 ====================

inline MinHeapTimer::MinHeapTimer() = default;
inline MinHeapTimer::~MinHeapTimer() = default;

inline uint64_t MinHeapTimer::add_timer(int64_t timeout_ms, TimerCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    int64_t expire = current_time_ms() + timeout_ms;

    heap_.push_back(TimerNode(id, expire, 0, std::move(callback)));
    size_t index = heap_.size() - 1;
    id_map_[id] = index;
    heapify_up(index);
    return id;
}

inline uint64_t MinHeapTimer::add_timer_repeat(int64_t interval_ms, TimerCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    int64_t expire = current_time_ms() + interval_ms;

    heap_.push_back(TimerNode(id, expire, interval_ms, std::move(callback)));
    size_t index = heap_.size() - 1;
    id_map_[id] = index;
    heapify_up(index);
    return id;
}

inline void MinHeapTimer::remove_timer(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = id_map_.find(id);
    if (it != id_map_.end()) {
        del(it->second);
        id_map_.erase(it);
    }
}

inline int64_t MinHeapTimer::tick() {
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t now = current_time_ms();
    size_t count = 0;

    while (!heap_.empty() && heap_.front().expire <= now) {
        TimerNode node = heap_.front();
        del(0);
        id_map_.erase(node.id);

        if (node.callback) {
            node.callback();
        }

        // 重复定时器：重新插入
        if (node.interval > 0) {
            node.expire = now + node.interval;
            heap_.push_back(node);
            id_map_[node.id] = heap_.size() - 1;
            heapify_up(heap_.size() - 1);
        }
        count++;
    }

    if (!heap_.empty()) {
        return heap_.front().expire - now;
    }
    return -1;
}

inline int64_t MinHeapTimer::current_time_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

inline size_t MinHeapTimer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}

inline void MinHeapTimer::heapify_up(size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (heap_[index] < heap_[parent]) {
            std::swap(heap_[index], heap_[parent]);
            id_map_[heap_[index].id] = index;
            id_map_[heap_[parent].id] = parent;
            index = parent;
        } else {
            break;
        }
    }
}

inline void MinHeapTimer::heapify_down(size_t index) {
    size_t n = heap_.size();
    while (true) {
        size_t smallest = index;
        size_t left = 2 * index + 1;
        size_t right = 2 * index + 2;

        if (left < n && heap_[left] < heap_[smallest]) smallest = left;
        if (right < n && heap_[right] < heap_[smallest]) smallest = right;

        if (smallest != index) {
            std::swap(heap_[index], heap_[smallest]);
            id_map_[heap_[index].id] = index;
            id_map_[heap_[smallest].id] = smallest;
            index = smallest;
        } else {
            break;
        }
    }
}

inline void MinHeapTimer::del(size_t index) {
    size_t n = heap_.size();
    assert(index < n);

    if (index == n - 1) {
        heap_.pop_back();
        return;
    }

    std::swap(heap_[index], heap_.back());
    id_map_[heap_[index].id] = index;
    heap_.pop_back();

    // 尝试上浮或下沉
    heapify_up(index);
    heapify_down(index);
}

// ==================== TimingWheel 实现 ====================

inline TimingWheel::TimingWheel(int64_t tick_ms, size_t wheel_size, int64_t max_timeout)
    : tick_ms_(tick_ms)
    , wheel_size_(wheel_size)
    , max_timeout_(max_timeout)
    , current_time_(MinHeapTimer::current_time_ms())
    , slots_(wheel_size, nullptr) {}

inline uint64_t TimingWheel::add_timer(int64_t timeout_ms, TimerCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (timeout_ms > max_timeout_) {
        timeout_ms = max_timeout_;
    }

    uint64_t id = next_id_++;
    int64_t expire = current_time_ + timeout_ms;
    size_t slot = static_cast<size_t>((current_time_ + timeout_ms) / tick_ms_) % wheel_size_;

    auto* node = new WheelNode(id, std::move(callback), expire);
    node->next = slots_[slot];
    slots_[slot] = node;
    node_map_[id] = node;

    return id;
}

inline void TimingWheel::remove_timer(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = node_map_.find(id);
    if (it != node_map_.end()) {
        // 从链表中移除（简化：标记为无效）
        it->second->callback = nullptr;
        node_map_.erase(it);
    }
}

inline size_t TimingWheel::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t slot = static_cast<size_t>(current_time_ / tick_ms_) % wheel_size_;
    size_t count = 0;

    WheelNode* node = slots_[slot];
    slots_[slot] = nullptr;

    while (node) {
        auto* next = node->next;
        if (node->callback) {
            node->callback();
            count++;
        }
        node_map_.erase(node->id);
        delete node;
        node = next;
    }

    current_time_ += tick_ms_;
    return count;
}

inline void TimingWheel::advance(int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (current_time_ < now) {
        tick();
    }
}

inline int64_t TimingWheel::next_timeout() const {
    return tick_ms_;
}

} // namespace high_perf

#endif // TIMER_HPP
