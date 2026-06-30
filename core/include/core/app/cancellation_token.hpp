// core/app/cancellation_token.hpp — concrete cooperative cancel token (REFACTOR_SPEC §7.4). Pure STL.
#pragma once

#include <atomic>

#include "core/domain/ports/driven/i_cancellation_token.hpp"

namespace core::app {

class CancellationToken final : public domain::ICancellationToken {
public:
  bool cancelled() const noexcept override { return flag_.load(std::memory_order_acquire); }
  void cancel() noexcept { flag_.store(true, std::memory_order_release); }

private:
  std::atomic<bool> flag_{false};
};

} // namespace core::app
