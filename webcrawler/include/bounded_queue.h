// bounded_queue.h — Thread-safe bounded queue with blocking push/pop
// and graceful shutdown support.
#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

template<typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity)
        : capacity_(capacity), shutdown_(false) {}

    // Push item; blocks if full. Returns false immediately if shut down.
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] {
            return queue_.size() < capacity_ || shutdown_;
        });
        if (shutdown_) return false;
        queue_.push(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    // Pop item; blocks if empty. Returns nullopt if shut down and empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] {
            return !queue_.empty() || shutdown_;
        });
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return item;
    }

    // Signal shutdown; all blocked callers will unblock.
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    bool is_shutdown() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdown_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::queue<T>           queue_;
    mutable std::mutex      mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    size_t                  capacity_;
    bool                    shutdown_;
};
