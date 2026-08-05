// core/app/cancellation_token.hpp — concrete cooperative cancel token (REFACTOR_SPEC §7.4). Pure STL.
// Flag + abort hooks: cancel() trips the flag AND fires every hook a sender registered for its live
// connection, so Cancel kills a hung transfer instead of waiting for a poll that never comes.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "core/domain/ports/driven/i_cancellation_token.hpp"

namespace core::app {

class CancellationToken final : public domain::ICancellationToken {
public:
  bool cancelled() const noexcept override { return flag_.load(std::memory_order_acquire); }

  // Idempotent, callable from any thread. Hooks run OUTSIDE the lock (they touch transport state).
  void cancel() {
    if (flag_.exchange(true, std::memory_order_acq_rel)) return;
    std::vector<std::function<void()>> hooks;
    { std::lock_guard<std::mutex> lk(mu_); hooks.swap(hooks_); }
    for (auto &h : hooks) if (h) h();
  }

  void onCancel(std::function<void()> hook) const override {
    if (!hook) return;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!cancelled()) { hooks_.push_back(std::move(hook)); return; }
    }
    hook(); // already cancelled -> run now, don't store
  }

private:
  std::atomic<bool> flag_{false};
  mutable std::mutex mu_;
  mutable std::vector<std::function<void()>> hooks_;
};

} // namespace core::app
