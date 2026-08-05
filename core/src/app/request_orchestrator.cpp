#include "core/app/request_orchestrator.hpp"

#include <string>
#include <utility>
#include <vector>

#include "core/domain/http/http_request.hpp" // acceptsEventStream (SSE classifier)

namespace core::app {
namespace d = core::domain;

namespace {

// Long-lived == the sender's execute() blocks indefinitely (until the user stops it / the peer closes),
// as opposed to a bounded call that returns once its own data is exhausted. Used ONLY to pick an executor
// (StreamPool vs the small unary ThreadPool) — NOT a general interaction classifier (see CoreApiClient::
// interactionOf for that). Erring towards "true" here is safe (just uses the bigger/unbounded pool for a
// request that happens to finish quickly); erring towards "false" is NOT (recreates the starvation bug).
bool isLongLivedSend(const d::RequestModel &request) {
  switch (request.type()) {
  case d::RequestType::WebSocket:
    return true; // duplex session, open until the user disconnects
  case d::RequestType::Grpc: {
    auto mt = std::get<d::GrpcRequest>(request.payload()).methodType();
    // Client-streaming sends a pre-built list of messages then awaits ONE response (bounded, like unary) —
    // only server-stream/bidi keep the call open waiting on the PEER indefinitely.
    return mt == d::GrpcMethodType::ServerStreaming || mt == d::GrpcMethodType::BidiStreaming;
  }
  case d::RequestType::GraphQl:
    // Only a ws-subTransport subscription is actually long-lived (native_graphql_sender.cpp's
    // runSubscription); a plain HTTP query/mutation is bounded. Approximated by subTransport alone (no
    // infra dependency to check the operation kind) — a ws-transport query/mutation would be slightly
    // over-classified as "streaming", which is the safe direction per the note above.
    return std::get<d::GraphQlRequest>(request.payload()).subTransport() == d::GqlSubTransport::Ws;
  case d::RequestType::Http:
    return d::acceptsEventStream(std::get<d::HttpRequest>(request.payload()));
  case d::RequestType::Kafka:
    return std::get<d::KafkaRequest>(request.payload()).kind() == d::KafkaClientKind::Consumer;
  case d::RequestType::Soap:
    return false; // one bounded HTTP POST
  }
  return false;
}

} // namespace

RequestOrchestrator::RequestOrchestrator(SendRequestSaga::Deps deps, Executor unaryExec, Executor streamExec)
    : deps_(std::move(deps)), unaryExecutor_(std::move(unaryExec)), streamExecutor_(std::move(streamExec)) {
  auto inlineExec = [](std::function<void()> job) { job(); };
  if (!unaryExecutor_) unaryExecutor_ = inlineExec;   // default: inline (synchronous), same as before the split
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
    std::lock_guard<std::mutex> lk(mu_); // drop terminal saga (lifecycle done)
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
