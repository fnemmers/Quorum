// thread_pool.h — Fixed-size thread pool backed by a BoundedQueue.
#pragma once
#include <vector>
#include <thread>
#include <functional>
#include <atomic>
#include "bounded_queue.h"

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t num_threads, size_t queue_capacity = 1024)
        : work_queue_(queue_capacity), active_(true) {
        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() { shutdown(); }

    // Submit a callable; blocks if queue is full.
    // Returns false if the pool has been shut down.
    bool submit(Task task) {
        return work_queue_.push(std::move(task));
    }

    void shutdown() {
        if (active_.exchange(false)) {
            work_queue_.shutdown();
            for (auto& t : workers_)
                if (t.joinable()) t.join();
        }
    }

    size_t pending() const { return work_queue_.size(); }

private:
    void worker_loop() {
        while (true) {
            auto task = work_queue_.pop();
            if (!task) break;
            (*task)();
        }
    }

    BoundedQueue<Task>       work_queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool>        active_;
};
