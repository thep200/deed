// core/app/request_orchestrator.hpp — saga coordinator, used concretely by CoreApiClient (REFACTOR_SPEC §7.3).
// Owns the live sagas keyed by RequestExecutionId, hands each to an executor, and serializes per-exec
// observer delivery (one saga = one task = naturally sequential). Pure domain/app deps only.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/app/send_request_saga.hpp"

namespace core::app {

// NOTE: does NOT implement domain::IApiClient (dropped — nothing ever held/passed a RequestOrchestrator
// polymorphically as IApiClient; every caller uses the concrete type directly, e.g.
// core_api_client.hpp's `std::unique_ptr<RequestOrchestrator>`). That forced inheritance is what made
// listGrpcMethods() mandatory here even though CoreApiClient (the real IApiClient) never delegates to it —
// removed as dead code (verified zero callers) rather than left as a permanent "Unsupported" stub.
class RequestOrchestrator final {
public:
  // Executor runs a unit of work; default = inline (synchronous). Production injects a thread pool so
  // sends run on background threads (observer marshals to the UI queue itself — §6.2).
  using Executor = std::function<void(std::function<void()>)>;

  // TWO executors (tech-debt fix): send() classifies the request and routes long-lived streaming sessions
  // (WS / gRPC server-stream+bidi / Kafka consumer) to `streamExec`, everything else to `unaryExec`. Before
  // this split both went through the SAME executor — an indefinite stream could occupy a worker of a small
  // bounded pool for its whole lifetime, starving queued unary sends behind it. Omitting either -> inline
  // (synchronous) for that category, same default as before the split.
  explicit RequestOrchestrator(SendRequestSaga::Deps deps, Executor unaryExec = {}, Executor streamExec = {});

  domain::Result<domain::RequestExecutionId>
  send(const domain::RequestModel &request, std::shared_ptr<domain::IRequestObserver> observer);

  domain::Status cancel(domain::RequestExecutionId exec);
  domain::Status sendStreamMessage(domain::RequestExecutionId exec, domain::WsMessage msg);
  domain::Status halfClose(domain::RequestExecutionId exec);
  domain::Status closeStream(domain::RequestExecutionId exec, int code, std::string reason);

  domain::Status validateJson(const domain::JsonText &);

private:
  std::shared_ptr<SendRequestSaga> find(const domain::RequestExecutionId &exec);

  SendRequestSaga::Deps deps_;
  Executor unaryExecutor_;
  Executor streamExecutor_;
  std::mutex mu_;
  std::unordered_map<domain::RequestExecutionId, std::shared_ptr<SendRequestSaga>> sagas_;
  unsigned long counter_ = 0;
};

} // namespace core::app
