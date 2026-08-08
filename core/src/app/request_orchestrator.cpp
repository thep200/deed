#include "core/app/request_orchestrator.hpp"

#include <string>
#include <utility>
#include <vector>

#include "core/domain/http/http_request.hpp"

namespace core::app {
namespace d = core::domain;

namespace {

// Long-lived == execute() blocks until user stop / peer close; picks the executor only, not interactionOf.
// Over-classifying is safe, erring "false" recreates pool starvation. One overload per type -> compile error for new types.
bool longLivedTyped(const d::HttpRequest &p) { return d::acceptsEventStream(p); }
bool longLivedTyped(const d::GrpcRequest &p) {
  auto mt = p.methodType();
  return mt == d::GrpcMethodType::ServerStreaming || mt == d::GrpcMethodType::BidiStreaming;
}
bool longLivedTyped(const d::GraphQlRequest &p) {
  return p.subTransport() == d::GqlSubTransport::Ws;
}
bool longLivedTyped(const d::WebSocketRequest &) { return true; }
bool longLivedTyped(const d::KafkaRequest &p) { return p.kind() == d::KafkaClientKind::Consumer; }
bool longLivedTyped(const d::SoapRequest &) { return false; }
bool longLivedTyped(const d::LdapRequest &) { return false; }

bool isLongLivedSend(const d::RequestModel &request) {
  return request.match([](const auto &p) { return longLivedTyped(p); });
}

} // namespace

RequestOrchestrator::RequestOrchestrator(SendRequestSaga::Deps deps, Executor unaryExec, Executor streamExec)
    : deps_(std::move(deps)), unaryExecutor_(std::move(unaryExec)), streamExecutor_(std::move(streamExec)) {
  auto inlineExec = [](std::function<void()> job) { job(); };
  if (!unaryExecutor_) unaryExecutor_ = inlineExec;
  if (!streamExecutor_) streamExecutor_ = inlineExec;
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

  Executor exe = isLongLivedSend(request) ? streamExecutor_ : unaryExecutor_;
  exe([this, saga, observer, exec]() {
    saga->run(*observer);
    std::lock_guard<std::mutex> lk(mu_);
    sagas_.erase(exec);
  });
  return d::Result<d::RequestExecutionId>::ok(exec);
}

d::Status RequestOrchestrator::cancel(d::RequestExecutionId exec) {
  if (auto s = find(exec)) { s->cancel(); return d::ok(); }
  return d::Status::fail({d::ErrorCode::NotFound, "no such execution"});
}

d::Status RequestOrchestrator::cancelAll() {
  std::vector<std::shared_ptr<SendRequestSaga>> live;
  { // copy out first: cancel() reaches into transport state and must not run under our map lock
    std::lock_guard<std::mutex> lk(mu_);
    live.reserve(sagas_.size());
    for (auto &kv : sagas_) live.push_back(kv.second);
  }
  for (auto &s : live) if (s) s->cancel();
  return d::ok();
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

} // namespace core::app
