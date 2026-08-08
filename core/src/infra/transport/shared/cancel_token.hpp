// Senders poll cancelled() and/or register an onCancel hook for an immediate stop. Thread-safe:
// cancel() may race with the sender on another thread.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "core/domain/ports/driven/i_cancellation_token.hpp"

namespace core {

class CancelToken {
public:
    bool cancelled() const noexcept { return flag_.load(std::memory_order_acquire); }
    bool isCancelled() const noexcept { return cancelled(); }   // legacy name (unary senders)

    // Sets the flag + fires every registered hook exactly once. Idempotent.
    void cancel() {
        if (flag_.exchange(true, std::memory_order_acq_rel)) return;
        std::vector<std::function<void()>> hooks;
        { std::lock_guard<std::mutex> lk(mu_); hooks.swap(hooks_); }
        for (auto& h : hooks) if (h) h();
    }

    // If already cancelled, the hook runs immediately (not stored).
    void onCancel(std::function<void()> hook) {
        if (!hook) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!cancelled()) { hooks_.push_back(std::move(hook)); return; }
        }
        hook();
    }

private:
    std::atomic<bool> flag_{false};
    std::mutex mu_;
    std::vector<std::function<void()>> hooks_;
};

// Fresh token per call (hook fires immediately if the caller is already cancelled). Never park the token
// in a sender member: instances are shared by concurrent requests, so a member slot routes cancel to
// whichever call started last.
inline std::shared_ptr<CancelToken> linkCancel(const domain::ICancellationToken& src) {
    auto t = std::make_shared<CancelToken>();
    src.onCancel([t] { t->cancel(); });
    return t;
}

} // namespace core
