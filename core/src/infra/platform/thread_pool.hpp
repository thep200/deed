#pragma once

#include <algorithm>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace core {

class ThreadPool {
public:
    // maxQueue bounds the backlog (0 = unbounded); submit() refuses past it. Only short unary sends
    // run here — streams get dedicated threads (StreamPool).
    explicit ThreadPool(unsigned n = 0, std::size_t maxQueue = 1024) : maxQueue_(maxQueue) {
        if (n == 0) n = std::max(2u, std::thread::hardware_concurrency());
        for (unsigned i = 0; i < n; ++i)
            workers_.emplace_back([this] { workerLoop(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Returns false if shutting down or the queue is full (caller decides the fallback).
    bool submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (stop_) return false;
            if (maxQueue_ && tasks_.size() >= maxQueue_) return false;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::size_t maxQueue_ = 0;
};

} // namespace core
