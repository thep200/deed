#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/app/send_request_saga.hpp"

namespace core::app {

class RequestOrchestrator final {
public:
  // Executor runs a unit of work; default = inline (synchronous).
  using Executor = std::function<void(std::function<void()>)>;

  // send() routes long-lived streaming sessions to streamExec, everything else to unaryExec; omitting either -> inline.
  explicit RequestOrchestrator(SendRequestSaga::Deps deps, Executor unaryExec = {}, Executor streamExec = {});

  domain::Result<domain::RequestExecutionId>
  send(const domain::RequestModel &request, std::shared_ptr<domain::IRequestObserver> observer);

  domain::Status cancel(domain::RequestExecutionId exec);
  // Cancel EVERY live saga — must not no-op when the exec handle is stale or not registered yet.
  domain::Status cancelAll();
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
