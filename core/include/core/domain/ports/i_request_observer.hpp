// core/domain/ports/i_request_observer.hpp — driven port App -> UI (REFACTOR_SPEC §6.2).
#pragma once

#include "core/domain/ports/exec_id.hpp"
#include "core/domain/response/response_event.hpp"

namespace core::domain {

class IRequestObserver {
public:
  virtual ~IRequestObserver() = default;
  // CALLED ON A BACKGROUND THREAD, serialized per-exec by the orchestrator. The UI adapter marshals to the
  // main queue (GCD). Must not throw.
  virtual void onEvent(RequestExecutionId exec, const ResponseEvent &ev) noexcept = 0;
};

} // namespace core::domain
