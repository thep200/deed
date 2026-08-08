// One fresh detached OS thread per long-lived stream, so an indefinite stream never occupies the bounded
// unary ThreadPool; maxConcurrent is only a runaway-caller safety net. The destructor blocks until every
// in-flight thread finishes, so deps a live stream references can't be destroyed under it.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>

namespace core {

class StreamPool {
public:
    explicit StreamPool(std::size_t maxConcurrent = 256) : maxConcurrent_(maxConcurrent) {}

    ~StreamPool() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return active_ == 0; });
    }

    StreamPool(const StreamPool&) = delete;
    StreamPool& operator=(const StreamPool&) = delete;

    // Returns false if at the concurrency cap (caller decides the fallback, e.g. run inline).
    bool submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (maxConcurrent_ && active_ >= maxConcurrent_) return false;
            ++active_;
        }
        std::thread([this, task = std::move(task)]() mutable {
            task();
            {
                std::lock_guard<std::mutex> lk(m_);
                --active_;
            }
            cv_.notify_all();
        }).detach();
        return true;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::size_t active_ = 0;
    std::size_t maxConcurrent_;
};

} // namespace core
