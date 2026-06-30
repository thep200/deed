// core/app/request_orchestrator.hpp — coordinator + IApiClient impl (REFACTOR_SPEC §7.3).
// Owns the live sagas keyed by RequestExecutionId, hands each to an executor, and serializes per-exec
// observer delivery (one saga = one task = naturally sequential). Pure domain/app deps only.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/app/send_request_saga.hpp"
#include "core/domain/ports/driving/i_api_client.hpp"

namespace core::app {

class RequestOrchestrator final : public domain::IApiClient {
public:
  // Executor runs a unit of work; default = inline (synchronous). Production injects a thread pool so
  // sends run on background threads (observer marshals to the UI queue itself — §6.2).
  using Executor = std::function<void(std::function<void()>)>;

  explicit RequestOrchestrator(SendRequestSaga::Deps deps, Executor exec = {});

  domain::Result<domain::RequestExecutionId>
  send(const domain::RequestModel &request, std::shared_ptr<domain::IRequestObserver> observer) override;

  domain::Status cancel(domain::RequestExecutionId exec) override;
  domain::Status sendStreamMessage(domain::RequestExecutionId exec, domain::WsMessage msg) override;
  domain::Status halfClose(domain::RequestExecutionId exec) override;
  domain::Status closeStream(domain::RequestExecutionId exec, int code, std::string reason) override;

  domain::Status validateJson(const domain::JsonText &) override;
  domain::Result<std::vector<domain::GrpcMethodDescriptor>>
  listGrpcMethods(const domain::GrpcRequest &) override;

private:
  std::shared_ptr<SendRequestSaga> find(const domain::RequestExecutionId &exec);

  SendRequestSaga::Deps deps_;
  Executor executor_;
  std::mutex mu_;
  std::unordered_map<domain::RequestExecutionId, std::shared_ptr<SendRequestSaga>> sagas_;
  unsigned long counter_ = 0;
};

} // namespace core::app
