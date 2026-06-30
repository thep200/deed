// core/app/send_request_saga.hpp — process manager for ONE send (REFACTOR_SPEC §7.2).
// Drives resolve -> validate -> pick sender -> execute, emitting ResponseEvents to the observer; writes the
// cache on completion. Touches NO JSON and NO transport lib — only domain objects + ports.
#pragma once

#include <atomic>
#include <vector>

#include "core/app/cancellation_token.hpp"
#include "core/domain/ports/exec_id.hpp"
#include "core/domain/ports/i_clock.hpp"
#include "core/domain/ports/i_json_validator.hpp"
#include "core/domain/ports/i_request_observer.hpp"
#include "core/domain/ports/i_request_sender.hpp"
#include "core/domain/ports/i_response_cache.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/ws/ws_message.hpp"

namespace core::app {

// Lifecycle phases (REFACTOR_SPEC §7.1). Exposed for tests/telemetry.
enum class SagaState { Idle, Validating, Preparing, Active, Streaming, Completed, Failed, Cancelled };

class SendRequestSaga {
public:
  struct Deps {
    std::vector<domain::IRequestSender *> senders; // registry; first that supports(type) wins
    domain::IClock *clock = nullptr;               // required
    domain::IJsonValidator *jsonValidator = nullptr; // optional (skip JSON validation if null)
    domain::IResponseCache *cache = nullptr;         // optional (skip caching if null)
  };

  SendRequestSaga(domain::RequestExecutionId exec, domain::RequestModel request, Deps deps);

  // Runs the lifecycle synchronously on the calling (worker) thread, emitting to `observer`.
  void run(domain::IRequestObserver &observer);

  // Cancel: trip the token AND, for a live duplex/stream session, ask the bound sender to close so a
  // blocked execute() (e.g. WebSocket) unblocks and run() can finish. Safe to call from another thread.
  void cancel();
  SagaState state() const noexcept { return state_; }
  bool terminal() const noexcept {
    return state_ == SagaState::Completed || state_ == SagaState::Failed ||
           state_ == SagaState::Cancelled;
  }

  // Interactive streaming pass-through to the active sender.
  domain::Status push(domain::WsMessage);
  domain::Status halfClose();
  domain::Status close(int code, std::string reason);

private:
  domain::RequestExecutionId exec_;
  domain::RequestModel request_;
  Deps deps_;
  CancellationToken token_;
  std::atomic<domain::IRequestSender *> sender_{nullptr}; // bound during run(); read by cancel/push (other thread)
  SagaState state_ = SagaState::Idle;
};

} // namespace core::app
