#include <cstdio>
#include <string>

#include "core/app/send_request_saga.hpp"
#include "saga_fakes.hpp"

using namespace core::domain;
using namespace sagatest;
using core::app::SagaState;
using core::app::SendRequestSaga;

static int sg_pass = 0, sg_fail = 0;
#define SG_CHECK(cond, msg)                                                                        \
  do {                                                                                             \
    if (cond) { ++sg_pass; }                                                                       \
    else { ++sg_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }             \
  } while (0)

// Producer is unary-like (EvStarted -> EvCompleted); broker-internal behavior is covered by the integration test.
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
  // Cache-write is generic on EvCompleted (not per-RequestType), so the producer IS cached; only the streaming consumer never caches.
  SG_CHECK(cache.puts == 1, "kafka producer: cached like any other unary EvCompleted (see comment above)");
}

// Consumer stream: EvStarted -> EvKafkaRecord*N -> EvClosed -> Completed (Stop and maxMessages look the same to the saga).
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

// Cancel before run() short-circuits; mid-poll cancellation is KafkaSender-internal, covered by the integration test.
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

static void test_oauth2_rewrites_to_bearer() {
  FakeClock clock; FakeCache cache;
  FakeSender sender;
  sender.script = {completed()};
  FakeTokenProvider tp;
  SendRequestSaga saga(RequestExecutionId("o1"), makeHttpOAuth2(),
                       {{&sender}, &clock, nullptr, &cache, &tp});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(tp.calls == 1, "oauth2: provider called once");
  SG_CHECK(sender.executed, "oauth2: sender executed");
  SG_CHECK(sender.seenAuth == Auth::bearer("tok123").take(), "oauth2: sender saw Bearer, not OAuth2");
  SG_CHECK(saga.state() == SagaState::Completed, "oauth2: completed");
}

static void test_oauth2_provider_failure_short_circuits() {
  FakeClock clock; FakeCache cache;
  FakeSender sender;
  FakeTokenProvider tp;
  tp.result = Result<std::string>::fail({ErrorCode::Network, "invalid_client", ""});
  SendRequestSaga saga(RequestExecutionId("o2"), makeHttpOAuth2(),
                       {{&sender}, &clock, nullptr, &cache, &tp});
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(!sender.executed, "oauth2 fail: sender never runs");
  SG_CHECK(obs.events.size() == 1 && isAt<EvFailed>(obs.events, 0), "oauth2 fail: EvFailed only");
  const auto *f = obs.events[0].get<EvFailed>();
  SG_CHECK(f && f->error.message.find("oauth2 token:") == 0, "oauth2 fail: message prefixed");
  SG_CHECK(cache.puts == 0, "oauth2 fail: no cache write");
}

static void test_oauth2_without_provider_unsupported() {
  FakeClock clock; FakeCache cache;
  FakeSender sender;
  SendRequestSaga saga(RequestExecutionId("o3"), makeHttpOAuth2(),
                       {{&sender}, &clock, nullptr, &cache}); // no tokenProvider wired
  FakeObserver obs;
  saga.run(obs);
  SG_CHECK(!sender.executed, "oauth2 no-provider: sender never runs");
  const auto *f = obs.events.size() == 1 ? obs.events[0].get<EvFailed>() : nullptr;
  SG_CHECK(f && f->error.kind == ErrorKind::Unsupported, "oauth2 no-provider: Unsupported");
}

int run_saga_protocol_tests() {
  std::printf("[saga_protocol]\n");
  test_kafka_producer_happy();
  test_kafka_consumer_streaming();
  test_kafka_consumer_cancel_before_run();
  test_oauth2_rewrites_to_bearer();
  test_oauth2_provider_failure_short_circuits();
  test_oauth2_without_provider_unsupported();
  std::printf("  saga_protocol: %d passed, %d failed\n", sg_pass, sg_fail);
  return sg_fail;
}
