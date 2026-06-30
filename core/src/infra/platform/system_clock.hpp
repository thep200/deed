// core/src/infra/platform/system_clock.hpp — IClock backed by std::chrono::steady_clock (REFACTOR_SPEC §8).
#pragma once

#include "core/domain/ports/driven/i_clock.hpp"

namespace core::infra {

class SystemClock final : public domain::IClock {
public:
  std::chrono::steady_clock::time_point now() const override {
    return std::chrono::steady_clock::now();
  }
};

} // namespace core::infra
