// saga_test.cpp — REFACTOR_SPEC §11.3 saga/orchestrator tests with fakes.
// FakeSender emits a scripted ResponseEvent sequence; FakeObserver records what the saga delivers; we
// assert event ordering follows the state machine, cancel/validation short-circuit before the sender,
// and the cache is written exactly once on EvCompleted (never on failure).
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/app/request_orchestrator.hpp"
#include "core/app/send_request_saga.hpp"
#include "core/domain/ports/i_clock.hpp"
#include "core/domain/ports/i_json_validator.hpp"
#include "core/domain/ports/i_request_observer.hpp"
#include "core/domain/ports/i_request_sender.hpp"
#include "core/domain/ports/i_response_cache.hpp"
#include "core/domain/request/request_model.hpp"

using namespace core::domain;
using core::app::RequestOrchestrator;
using core::app::SagaState;
using core::app::SendRequestSaga;

static int sg_pass = 0, sg_fail = 0;
#define SG_CHECK(cond, msg)                                                                        \
  do {                                                                                             \
    if (cond) { ++sg_pass; }                                                                       \
    else { ++sg_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }             \
  } while (0)

namespace {

struct FakeClock final : IClock {
  std::chrono::steady_clock::time_point now() const override {
    return std::chrono::steady_clock::time_point(std::chrono::milliseconds(1000));
  }
};

struct FakeObserver final : IRequestObserver {
  std::vector<ResponseEvent> events;
  void onEvent(RequestExecutionId, const ResponseEvent &ev) noexcept override { events.push_back(ev); }
};

struct FakeCache final : IResponseCache {
  int puts = 0;
  void put(const RequestId &, const ApiResponse &) override { ++puts; }
  std::optional<ApiResponse> get(const RequestId &) const override { return std::nullopt; }
};

struct FakeValidator final : IJsonValidator {
  bool ok = true;
  Status validate(const JsonText &) const override {
    return ok ? core::domain::ok() : Status::fail({ErrorCode::Parse, "bad json", "body.json"});
  }
};

struct FakeSender final : IRequestSender {
  RequestType type = RequestType::Http;
  std::vector<ResponseEvent> script;
  bool returnFail = false;
  bool executed = false;
  bool supports(RequestType t) const override { return t == type; }
  Status execute(const RequestModel &, IResponseSink &sink, const ICancellationToken &) override {
    executed = true;
    for (const auto &ev : script) sink.emit(ev);
    return returnFail ? Status::fail({ErrorCode::Network, "boom"}) : core::domain::ok();
  }
};

RequestModel makeHttp(Body body = Body::none()) {
  HttpRequest::Parts p{HttpMethod::Get, Url::create("https://h/x").take()};
  p.body = std::move(body);
  RequestConfig cfg{Timeout::fromMillis(1000).take(), true};
  return RequestModel::create(RequestId("req_1"), "R", 0, cfg, HttpRequest::create(std::move(p)).take())
      .take();
}

ResponseEvent completed() {
  ApiResponse r;
  r.statusCode = 200;
  r.body = "ok";
  return ResponseEvent(EvCompleted{r});
}

template <class T> bool isAt(const std::vector<ResponseEvent> &v, size_t i) {
  return i < v.size() && v[i].is<T>();
}

} // namespace

static void test_unary_happy() {
  FakeClock clock; FakeCache cache;
  FakeSender sender;
  sender.script = {ResponseEvent(EvMetadata{}), completed()};
  SendRequestSaga saga(RequestExecutionId("e1"), makeHttp(),
                       {{&sender}, &clock, nullptr, &cache});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(obs.events.size() == 3, "happy: 3 events");
  SG_CHECK(isAt<EvStarted>(obs.events, 0), "happy: EvStarted first");
  SG_CHECK(isAt<EvMetadata>(obs.events, 1), "happy: EvMetadata second");
  SG_CHECK(isAt<EvCompleted>(obs.events, 2), "happy: EvCompleted last");
  SG_CHECK(saga.state() == SagaState::Completed, "happy: state Completed");
  SG_CHECK(cache.puts == 1, "happy: cache written once");
}

static void test_streaming_order() {
  FakeClock clock; FakeCache cache;
  FakeSender sender;
  sender.script = {ResponseEvent(EvMessage{WsSendKind::Text, "a", 0}),
                   ResponseEvent(EvMessage{WsSendKind::Text, "b", 1}), completed()};
  SendRequestSaga saga(RequestExecutionId("e2"), makeHttp(), {{&sender}, &clock, nullptr, &cache});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(obs.events.size() == 4, "stream: 4 events");
  SG_CHECK(isAt<EvMessage>(obs.events, 1) && isAt<EvMessage>(obs.events, 2), "stream: 2 messages");
  SG_CHECK(isAt<EvCompleted>(obs.events, 3), "stream: completed last");
  SG_CHECK(cache.puts == 1, "stream: cache once");
}

static void test_sender_failure_no_cache() {
  FakeClock clock; FakeCache cache;
  FakeSender sender;
  sender.script = {ResponseEvent(EvFailed{{ErrorKind::Network, "down", {}}})};
  SendRequestSaga saga(RequestExecutionId("e3"), makeHttp(), {{&sender}, &clock, nullptr, &cache});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(isAt<EvFailed>(obs.events, 1), "fail: EvFailed delivered");
  SG_CHECK(saga.state() == SagaState::Failed, "fail: state Failed");
  SG_CHECK(cache.puts == 0, "fail: no cache write");
}

static void test_timeout_forwarded() {
  FakeClock clock; FakeCache cache;
  FakeSender sender;
  sender.script = {ResponseEvent(EvFailed{{ErrorKind::Timeout, "deadline", {}}})};
  SendRequestSaga saga(RequestExecutionId("e4"), makeHttp(), {{&sender}, &clock, nullptr, &cache});
  FakeObserver obs;
  saga.run(obs);
  const auto *f = obs.events.back().get<EvFailed>();
  SG_CHECK(f && f->error.kind == ErrorKind::Timeout, "timeout: forwarded as EvFailed{Timeout}");
  SG_CHECK(cache.puts == 0, "timeout: no cache");
}

static void test_validation_short_circuits() {
  FakeClock clock; FakeCache cache;
  FakeValidator validator; validator.ok = false;
  FakeSender sender;
  sender.script = {completed()};
  SendRequestSaga saga(RequestExecutionId("e5"), makeHttp(Body::raw(RawSubtype::Json, "{bad")),
                       {{&sender}, &clock, &validator, &cache});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(!sender.executed, "validate: sender not invoked on bad json");
  const auto *f = obs.events.back().get<EvFailed>();
  SG_CHECK(f && f->error.kind == ErrorKind::Parse, "validate: EvFailed{Parse}");
  SG_CHECK(cache.puts == 0, "validate: no cache");
}

static void test_unsupported_type() {
  FakeClock clock;
  FakeSender sender; sender.type = RequestType::Grpc; // does not support http
  SendRequestSaga saga(RequestExecutionId("e6"), makeHttp(), {{&sender}, &clock, nullptr, nullptr});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(!sender.executed, "unsupported: sender not invoked");
  const auto *f = obs.events.back().get<EvFailed>();
  SG_CHECK(f && f->error.kind == ErrorKind::Unsupported, "unsupported: EvFailed{Unsupported}");
}

static void test_cancel_before_run() {
  FakeClock clock;
  FakeSender sender; sender.script = {completed()};
  SendRequestSaga saga(RequestExecutionId("e7"), makeHttp(), {{&sender}, &clock, nullptr, nullptr});
  saga.cancel();
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(!sender.executed, "cancel: sender not invoked");
  SG_CHECK(saga.state() == SagaState::Cancelled, "cancel: state Cancelled");
  const auto *f = obs.events.back().get<EvFailed>();
  SG_CHECK(f && f->error.kind == ErrorKind::Cancelled, "cancel: EvFailed{Cancelled}");
}

static void test_orchestrator_inline() {
  FakeClock clock; FakeCache cache;
  FakeSender sender; sender.script = {completed()};
  RequestOrchestrator orch({{&sender}, &clock, nullptr, &cache}); // inline executor (default)
  auto obs = std::make_shared<FakeObserver>();
  auto exec = orch.send(makeHttp(), obs);
  SG_CHECK(exec.isOk(), "orch: send returns exec id");
  SG_CHECK(!obs->events.empty() && obs->events.back().is<EvCompleted>(), "orch: completed delivered");
  // saga removed after terminal -> cancel an unknown/finished exec is NotFound.
  SG_CHECK(!orch.cancel(exec.value()).isOk(), "orch: finished exec no longer cancellable");
}

int run_saga_tests() {
  std::printf("[saga]\n");
  test_unary_happy();
  test_streaming_order();
  test_sender_failure_no_cache();
  test_timeout_forwarded();
  test_validation_short_circuits();
  test_unsupported_type();
  test_cancel_before_run();
  test_orchestrator_inline();
  std::printf("  saga: %d passed, %d failed\n", sg_pass, sg_fail);
  return sg_fail;
}
