#pragma once

#include <chrono>

namespace core::domain {

class IClock {
public:
  virtual ~IClock() = default;
  virtual std::chrono::steady_clock::time_point now() const = 0;
};

} // namespace core::domain
