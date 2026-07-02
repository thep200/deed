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
#include "core/domain/kafka/kafka_request.hpp"
#include "core/domain/ports/driven/i_clock.hpp"
#include "core/domain/ports/driven/i_json_validator.hpp"
#include "core/domain/ports/driven/i_request_observer.hpp"
#include "core/domain/ports/driven/i_request_sender.hpp"
#include "core/domain/ports/driven/i_response_cache.hpp"
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

RequestModel makeKafkaProducer() {
  auto brokers = BrokerList::parse("localhost:9092").take();
  KafkaProduceConfig cfg{KafkaTopic::create("demo-topic").take()};
  KafkaMessage msg;
  msg.value = MessagePayload{"{}", false};
  auto req = KafkaRequest::create(brokers, KafkaSecurity::plaintext(),
                                  KafkaRequest::Mode{KafkaProduceSpec{cfg, msg}})
                 .take();
  RequestConfig cfg2{Timeout::fromMillis(1800000).take(), false};
  return RequestModel::create(RequestId("req_kafka_p"), "P", 0, cfg2, req).take();
}

RequestModel makeKafkaConsumer() {
  auto brokers = BrokerList::parse("localhost:9092").take();
  KafkaConsumeConfig cfg{{KafkaTopic::create("demo-topic").take()}, std::nullopt,
                        ConsumerGroup::create("deed-tail-test").take()};
  auto req =
      KafkaRequest::create(brokers, KafkaSecurity::plaintext(), KafkaRequest::Mode{KafkaConsumeSpec{cfg}}).take();
  RequestConfig cfg2{Timeout::fromMillis(1800000).take(), false};
  return RequestModel::create(RequestId("req_kafka_c"), "C", 0, cfg2, req).take();
}

ResponseEvent kafkaRecord(int offset) {
  KafkaRecord r;
  r.topic = "demo-topic";
  r.partition = 0;
  r.offset = offset;
  r.value = "{}";
  return ResponseEvent(EvKafkaRecord{r});
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

// Tech-debt fix: RequestOrchestrator used to run EVERY send through the SAME executor, so a long-lived
// stream (WS/gRPC server-stream+bidi/Kafka consumer) sharing a small bounded pool with unary sends could
// occupy a worker indefinitely and starve queued unary requests behind it. Verifies the ROUTING decision
// deterministically (which executor gets invoked per request shape) rather than with real timing/threads,
// which would be flaky. See composition_root.cpp for the real wiring (ThreadPool vs StreamPool).
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

// SPEC_kafka §5/§11: producer is unary-like (EvStarted -> EvCompleted); the fakes below cover the saga's
// generic state-machine behavior end to end (real _PARTITION_EOF-not-an-error / cancel-mid-poll are
// KafkaSender-internal, exercised only by the integration test against a real broker, per spec §11's own
// split between the saga+fakes layer and the integration layer).
static void test_kafka_producer_happy() {
  FakeClock clock; FakeCache cache;
  FakeSender sender; sender.type = RequestType::Kafka;
  sender.script = {completed()};
  SendRequestSaga saga(RequestExecutionId("k1"), makeKafkaProducer(), {{&sender}, &clock, nullptr, &cache});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(isAt<EvStarted>(obs.events, 0), "kafka producer: EvStarted first");
  SG_CHECK(isAt<EvCompleted>(obs.events, 1), "kafka producer: EvCompleted last");
  SG_CHECK(saga.state() == SagaState::Completed, "kafka producer: state Completed");
  // Deviation from SPEC_kafka §5 ("cả 2 chế độ không ghi cache"): the saga's cache-write is a GENERIC
  // rule keyed on the event type (any EvCompleted is cached), not on RequestType — carving out "except
  // Kafka" would be the first type-specific branch in an otherwise protocol-agnostic saga. A cached
  // delivery-report summary is harmless and consistent with how every other unary type (HTTP/gRPC/GraphQL
  // query) already behaves, so producer IS cached; only the streaming consumer never caches (it emits
  // EvKafkaRecord/EvClosed, never EvCompleted — the same mechanism that already keeps WS/SSE cache-free).
  SG_CHECK(cache.puts == 1, "kafka producer: cached like any other unary EvCompleted (see comment above)");
}

// Consumer streaming: EvStarted -> EvKafkaRecord*N -> EvClosed. state()==Streaming while records flow;
// EvClosed (whether from Stop or maxMessages reached — both look the same to the saga) -> Completed.
static void test_kafka_consumer_streaming() {
  FakeClock clock; FakeCache cache;
  FakeSender sender; sender.type = RequestType::Kafka;
  sender.script = {kafkaRecord(0), kafkaRecord(1), kafkaRecord(2),
                   ResponseEvent(EvClosed{std::nullopt, "stopped"})};
  SendRequestSaga saga(RequestExecutionId("k2"), makeKafkaConsumer(), {{&sender}, &clock, nullptr, &cache});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(obs.events.size() == 5, "kafka consumer: EvStarted + 3 records + EvClosed");
  SG_CHECK(isAt<EvStarted>(obs.events, 0), "kafka consumer: EvStarted first");
  SG_CHECK(isAt<EvKafkaRecord>(obs.events, 1) && isAt<EvKafkaRecord>(obs.events, 2) &&
               isAt<EvKafkaRecord>(obs.events, 3),
           "kafka consumer: 3 EvKafkaRecord in order");
  SG_CHECK(isAt<EvClosed>(obs.events, 4), "kafka consumer: EvClosed last");
  SG_CHECK(saga.state() == SagaState::Completed, "kafka consumer: EvClosed -> state Completed");
  SG_CHECK(cache.puts == 0, "kafka consumer: no cache write (stream, spec §5)");
}

// Cancel BEFORE run() short-circuits before the sender is ever invoked (same generic guarantee HTTP/WS get;
// mid-poll cancellation is KafkaSender-internal cooperative-poll behavior, covered by the integration test).
static void test_kafka_consumer_cancel_before_run() {
  FakeClock clock;
  FakeSender sender; sender.type = RequestType::Kafka;
  sender.script = {kafkaRecord(0), ResponseEvent(EvClosed{std::nullopt, "stopped"})};
  SendRequestSaga saga(RequestExecutionId("k3"), makeKafkaConsumer(), {{&sender}, &clock, nullptr, nullptr});
  saga.cancel();
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(!sender.executed, "kafka consumer cancel: sender not invoked");
  SG_CHECK(saga.state() == SagaState::Cancelled, "kafka consumer cancel: state Cancelled");
  const auto *f = obs.events.back().get<EvFailed>();
  SG_CHECK(f && f->error.kind == ErrorKind::Cancelled, "kafka consumer cancel: EvFailed{Cancelled}");
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
  test_kafka_producer_happy();
  test_kafka_consumer_streaming();
  test_kafka_consumer_cancel_before_run();
  std::printf("  saga: %d passed, %d failed\n", sg_pass, sg_fail);
  return sg_fail;
}
