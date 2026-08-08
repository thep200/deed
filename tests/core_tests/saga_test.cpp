#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/app/request_orchestrator.hpp"
#include "core/app/send_request_saga.hpp"
#include "saga_fakes.hpp"

using namespace core::domain;
using namespace sagatest;
using core::app::RequestOrchestrator;
using core::app::SagaState;
using core::app::SendRequestSaga;

static int sg_pass = 0, sg_fail = 0;
#define SG_CHECK(cond, msg)                                                                        \
  do {                                                                                             \
    if (cond) { ++sg_pass; }                                                                       \
    else { ++sg_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }             \
  } while (0)

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

// Pins executor ROUTING per request shape (deterministic, no timing): long-lived streams must not starve the bounded unary pool.
static void test_orchestrator_routes_unary_vs_stream_executor() {
  FakeClock clock;
  FakeSender http, ws, grpc, gql, kafka;
  ws.type = RequestType::WebSocket;
  ws.script = {completed()};
  grpc.type = RequestType::Grpc;
  grpc.script = {completed()};
  gql.type = RequestType::GraphQl;
  gql.script = {completed()};
  kafka.type = RequestType::Kafka;
  kafka.script = {completed()};
  http.script = {completed()};

  int unaryCalls = 0, streamCalls = 0;
  RequestOrchestrator orch(
      {{&http, &ws, &grpc, &gql, &kafka}, &clock, nullptr, nullptr},
      [&](std::function<void()> job) { ++unaryCalls; job(); },
      [&](std::function<void()> job) { ++streamCalls; job(); });
  auto obs = std::make_shared<FakeObserver>();

  auto route = [&](RequestModel m) {
    unaryCalls = streamCalls = 0;
    orch.send(m, obs);
    return streamCalls > 0 ? "stream" : (unaryCalls > 0 ? "unary" : "neither");
  };

  SG_CHECK(std::string(route(makeHttp())) == "unary", "route: HTTP -> unary executor");
  SG_CHECK(std::string(route(makeKafkaProducer())) == "unary", "route: Kafka producer -> unary executor");

  auto mkGrpc = [](GrpcMethodType mt) {
    GrpcRequest::Parts p; p.target = "h:1"; p.methodType = mt;
    RequestConfig cfg{Timeout::fromMillis(1000).take(), true};
    return RequestModel::create(RequestId("g"), "G", 0, cfg, GrpcRequest::create(std::move(p)).take()).take();
  };
  SG_CHECK(std::string(route(mkGrpc(GrpcMethodType::Unary))) == "unary", "route: gRPC unary -> unary executor");
  SG_CHECK(std::string(route(mkGrpc(GrpcMethodType::ClientStreaming))) == "unary",
           "route: gRPC client-streaming -> unary executor (bounded, not indefinite)");
  SG_CHECK(std::string(route(mkGrpc(GrpcMethodType::ServerStreaming))) == "stream",
           "route: gRPC server-streaming -> stream executor");
  SG_CHECK(std::string(route(mkGrpc(GrpcMethodType::BidiStreaming))) == "stream",
           "route: gRPC bidi -> stream executor");

  RequestConfig cfg{Timeout::fromMillis(1000).take(), true};
  WebSocketRequest::Parts wp{Url::create("wss://h/s").take()};
  auto wsModel = RequestModel::create(RequestId("w"), "W", 0, cfg, WebSocketRequest::create(std::move(wp)).take()).take();
  SG_CHECK(std::string(route(wsModel)) == "stream", "route: WebSocket -> stream executor (duplex, indefinite)");

  GraphQlRequest::Parts gp{Url::create("https://h/gql").take(), {}, {}, Auth::none(), GqlSubTransport::Http, ""};
  gp.op.query = "query { x }";
  auto gqlHttpModel = RequestModel::create(RequestId("q1"), "Q", 0, cfg, GraphQlRequest::create(gp).take()).take();
  SG_CHECK(std::string(route(gqlHttpModel)) == "unary", "route: GraphQL over HTTP -> unary executor");
  gp.subTransport = GqlSubTransport::Ws;
  gp.wsProtocol = "graphql-transport-ws";
  auto gqlWsModel = RequestModel::create(RequestId("q2"), "Q", 0, cfg, GraphQlRequest::create(gp).take()).take();
  SG_CHECK(std::string(route(gqlWsModel)) == "stream", "route: GraphQL over WS -> stream executor");

  SG_CHECK(std::string(route(makeKafkaConsumer())) == "stream", "route: Kafka consumer -> stream executor");
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
  test_orchestrator_routes_unary_vs_stream_executor();
  std::printf("  saga: %d passed, %d failed\n", sg_pass, sg_fail);
  return sg_fail;
}
