#pragma once

#include "core/domain/ports/driving/exec_id.hpp"
#include "core/domain/response/response_event.hpp"

namespace core::domain {

class IRequestObserver {
public:
  virtual ~IRequestObserver() = default;
  // CALLED ON A BACKGROUND THREAD, serialized per-exec; the UI adapter marshals to the main queue. Must not throw.
  virtual void onEvent(RequestExecutionId exec, const ResponseEvent &ev) noexcept = 0;
};

} // namespace core::domain
