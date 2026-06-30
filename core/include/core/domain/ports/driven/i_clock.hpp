// core/domain/ports/driven/i_clock.hpp — injectable monotonic clock (REFACTOR_SPEC §6.3) so timeout logic is testable.
#pragma once

#include <chrono>

namespace core::domain {

class IClock {
public:
  virtual ~IClock() = default;
  virtual std::chrono::steady_clock::time_point now() const = 0;
};

} // namespace core::domain
