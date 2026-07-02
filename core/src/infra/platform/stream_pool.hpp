// stream_pool.hpp — one dedicated OS thread per long-lived streaming session (WS / gRPC server-stream+bidi /
// Kafka consumer), so a slow or indefinite stream never occupies a worker of the bounded unary ThreadPool
// behind it. This is exactly what ThreadPool's own doc comment always assumed ("Streams/sessions no longer
// run here (they get dedicated threads)") — RequestOrchestrator just wasn't actually routing them there.
//
// Unlike ThreadPool there's no fixed worker count: submit() spawns a fresh detached thread per task.
// maxConcurrent is a safety net (H1a-style) against a runaway caller opening unbounded sessions — past the
// cap, submit() returns false and the caller falls back (same contract as ThreadPool::submit).
//
// Shutdown semantics deliberately mirror ThreadPool: the destructor blocks until every in-flight thread has
// finished, so a caller (CoreApiClient) can't destroy the senders/saga deps a live thread still references.
// This means destruction can block if a stream is still open when the pool is torn down — the exact same
// pre-existing behavior ThreadPool already has today (its destructor joins each worker's current task before
// returning); splitting the pool does not make this better or worse, only isolates it from unary requests.
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
