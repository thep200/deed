#include "core/app/request_orchestrator.hpp"

#include <string>
#include <utility>

namespace core::app {
namespace d = core::domain;

RequestOrchestrator::RequestOrchestrator(SendRequestSaga::Deps deps, Executor exec)
    : deps_(std::move(deps)), executor_(std::move(exec)) {
  if (!executor_) executor_ = [](std::function<void()> job) { job(); }; // default: inline
}

std::shared_ptr<SendRequestSaga> RequestOrchestrator::find(const d::RequestExecutionId &exec) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = sagas_.find(exec);
  return it == sagas_.end() ? nullptr : it->second;
}

d::Result<d::RequestExecutionId>
RequestOrchestrator::send(const d::RequestModel &request,
                          std::shared_ptr<d::IRequestObserver> observer) {
  if (!observer)
    return d::Result<d::RequestExecutionId>::fail({d::ErrorCode::Validation, "observer required"});

  d::RequestExecutionId exec("");
  std::shared_ptr<SendRequestSaga> saga;
  {
    std::lock_guard<std::mutex> lk(mu_);
    exec = d::RequestExecutionId("exec_" + std::to_string(++counter_));
    saga = std::make_shared<SendRequestSaga>(exec, request, deps_);
    sagas_[exec] = saga;
  }

  Executor exe = executor_;
  exe([this, saga, observer, exec]() {
    saga->run(*observer);
    std::lock_guard<std::mutex> lk(mu_); // drop terminal saga (lifecycle done)
    sagas_.erase(exec);
  });
  return d::Result<d::RequestExecutionId>::ok(exec);
}

d::Status RequestOrchestrator::cancel(d::RequestExecutionId exec) {
  if (auto s = find(exec)) { s->cancel(); return d::ok(); }
  return d::Status::fail({d::ErrorCode::NotFound, "no such execution"});
}

d::Status RequestOrchestrator::sendStreamMessage(d::RequestExecutionId exec, d::WsMessage msg) {
  if (auto s = find(exec)) return s->push(std::move(msg));
  return d::Status::fail({d::ErrorCode::NotFound, "no such execution"});
}

d::Status RequestOrchestrator::halfClose(d::RequestExecutionId exec) {
  if (auto s = find(exec)) return s->halfClose();
  return d::Status::fail({d::ErrorCode::NotFound, "no such execution"});
}

d::Status RequestOrchestrator::closeStream(d::RequestExecutionId exec, int code, std::string reason) {
  if (auto s = find(exec)) return s->close(code, std::move(reason));
  return d::Status::fail({d::ErrorCode::NotFound, "no such execution"});
}

d::Status RequestOrchestrator::validateJson(const d::JsonText &text) {
  if (deps_.jsonValidator) return deps_.jsonValidator->validate(text);
  return d::ok();
}

d::Result<std::vector<d::GrpcMethodDescriptor>>
RequestOrchestrator::listGrpcMethods(const d::GrpcRequest &) {
  // Requires a gRPC reflection/descriptor infra adapter (REFACTOR_SPEC P5). Not wired yet.
  return d::Result<std::vector<d::GrpcMethodDescriptor>>::fail(
      {d::ErrorCode::Unsupported, "listGrpcMethods not wired (P5)"});
}

} // namespace core::app
