#pragma once

#include <atomic>
#include <vector>

#include "core/app/cancellation_token.hpp"
#include "core/domain/ports/driving/exec_id.hpp"
#include "core/domain/ports/driven/i_clock.hpp"
#include "core/domain/ports/driven/i_json_validator.hpp"
#include "core/domain/ports/driven/i_request_observer.hpp"
#include "core/domain/ports/driven/i_request_sender.hpp"
#include "core/domain/ports/driven/i_response_cache.hpp"
#include "core/domain/ports/driven/i_token_provider.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/ws/ws_message.hpp"

namespace core::app {

enum class SagaState { Idle, Validating, Preparing, Active, Streaming, Completed, Failed, Cancelled };

class SendRequestSaga {
public:
  struct Deps {
    std::vector<domain::IRequestSender *> senders; // registry; first that supports(type) wins
    domain::IClock *clock = nullptr;               // required
    domain::IJsonValidator *jsonValidator = nullptr; // optional (skip JSON validation if null)
    domain::IResponseCache *cache = nullptr;         // optional (skip caching if null)
    domain::ITokenProvider *tokenProvider = nullptr; // optional (an oauth2 request FAILS without it)
  };

  SendRequestSaga(domain::RequestExecutionId exec, domain::RequestModel request, Deps deps);

  // Runs the lifecycle synchronously on the calling (worker) thread, emitting to `observer`.
  void run(domain::IRequestObserver &observer);

  // Trips the token AND closes a live stream so a blocked execute() unblocks; safe from another thread.
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
