// core/domain/ports/driven/i_cancellation_token.hpp — cooperative cancel signal (REFACTOR_SPEC §6.3/§7.4).
#pragma once

#include <functional>

namespace core::domain {

class ICancellationToken {
public:
  virtual ~ICancellationToken() = default;
  virtual bool cancelled() const noexcept = 0;

  // Abort hook, fired once when cancel trips (runs NOW if already cancelled). Sender kills its live
  // socket/call here — polling alone never unblocks a syscall already parked on a dead peer.
  virtual void onCancel(std::function<void()> hook) const = 0;
};

// For call sites with no cancel channel (introspection, schema fetch). Never trips -> hooks are dropped.
class NoCancel final : public ICancellationToken {
public:
  bool cancelled() const noexcept override { return false; }
  void onCancel(std::function<void()>) const override {}
};

} // namespace core::domain
