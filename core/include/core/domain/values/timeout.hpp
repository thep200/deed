#pragma once

#include <chrono>

#include "core/domain/common/result.hpp"

namespace core::domain {

class Timeout {
public:
  // Invariant: strictly positive.
  static Result<Timeout> fromMillis(long long ms) {
    if (ms <= 0)
      return Result<Timeout>::fail({ErrorCode::Validation, "timeout must be > 0ms", "config.timeout_ms"});
    return Result<Timeout>::ok(Timeout(std::chrono::milliseconds(ms)));
  }

  std::chrono::milliseconds value() const noexcept { return ms_; }
  long long millis() const noexcept { return ms_.count(); }

  bool operator==(const Timeout &o) const { return ms_ == o.ms_; }
  bool operator!=(const Timeout &o) const { return ms_ != o.ms_; }

private:
  explicit Timeout(std::chrono::milliseconds ms) : ms_(ms) {}
  std::chrono::milliseconds ms_;
};

} // namespace core::domain
