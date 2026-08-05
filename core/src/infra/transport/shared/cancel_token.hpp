// infra/transport/shared/cancel_token.hpp — cancel signal shared between Engine and a sender (SPEC_grpc_streaming §5).
// Senders poll cancelled() in their loop AND/OR register an onCancel hook (e.g. ctx.TryCancel()) for an
// immediate stop. Thread-safe: cancel() may race with the sender on another thread.
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

    // Set the flag + fire every registered hook exactly once. Idempotent.
    void cancel() {
        if (flag_.exchange(true, std::memory_order_acq_rel)) return;   // already cancelled
        std::vector<std::function<void()>> hooks;
        { std::lock_guard<std::mutex> lk(mu_); hooks.swap(hooks_); }
        for (auto& h : hooks) if (h) h();
    }

    // Register a hook. If already cancelled, run it immediately (don't store).
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

// Bind a caller's domain token to a fresh token for THIS call. The hook fires immediately when the caller
// is already cancelled, so the pre-flight check is covered too. Senders use this instead of parking the
// token in a member: sender instances are SHARED by every concurrent request, so a member slot routes a
// cancel to whichever call happened to start last (and a finishing call clears it -> Cancel does nothing).
inline std::shared_ptr<CancelToken> linkCancel(const domain::ICancellationToken& src) {
    auto t = std::make_shared<CancelToken>();
    src.onCancel([t] { t->cancel(); });
    return t;
}

} // namespace core
