// kafka_manual_test.cpp — MANUAL integration check of the consumer cancel/timeout contract (SPEC_kafka
// §5/§7/§11's integration layer — needs a real broker, so NOT registered as a ctest). Drives KafkaSender
// directly against localhost:9092 (devdok up -d kafka).
// Usage: kafka_manual off   (run with the broker STOPPED)
//        kafka_manual on    (run with the broker UP; needs topic demo-topic)
// Contract (phase-based outcome):
//   off 1: unreachable broker, no cancel  -> EvFailed{Network}      (FAILURE after timeout_ms)
//   off 2: cancel while connecting        -> EvFailed{Cancelled}    (FAILURE)
//   on  3: connected, session timeout     -> EvClosed{"timeout"}    (SUCCESS)
//   on  4: cancel while consuming         -> EvClosed{"cancelled"}  (SUCCESS)
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core/app/cancellation_token.hpp"
#include "core/domain/kafka/kafka_request.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/response/response_event.hpp"
#include "infra/transport/kafka/kafka_sender.hpp"

using namespace core::domain;
using Clock = std::chrono::steady_clock;

namespace {

struct RecordingSink final : IResponseSink {
  std::string terminal;   // "failed:<kind>" | "closed:<reason>"
  std::string detail;     // failure message (diagnosis)
  int records = 0;
  void emit(const ResponseEvent &ev) override {
    if (const auto *f = ev.get<EvFailed>()) {
      terminal = "failed:" + toString(f->error.kind);
      detail = f->error.message;
    }
    else if (const auto *c = ev.get<EvClosed>()) terminal = "closed:" + c->reason;
    else if (ev.is<EvKafkaRecord>()) ++records;
  }
};

RequestModel makeConsumer(long long timeoutMs, const std::string &broker = "localhost:9092") {
  auto brokers = BrokerList::parse(broker).take();
  // assign(partition 0) + Earliest: records flow immediately, no group-join delay — the point here is
  // the cancel/timeout contract, not group semantics.
  KafkaConsumeConfig cfg{{KafkaTopic::create("demo-topic").take()}, KafkaPartition{0},
                        ConsumerGroup::create("deed-manual-test").take()};
  cfg.offsetReset = OffsetReset::Earliest;
  auto req =
      KafkaRequest::create(brokers, KafkaSecurity::plaintext(), KafkaRequest::Mode{KafkaConsumeSpec{cfg}}).take();
  RequestConfig rc{Timeout::fromMillis(timeoutMs).take(), false};
  return RequestModel::create(RequestId("req_manual"), "M", 0, rc, req).take();
}

// Run one scenario: execute the consumer with `timeoutMs`; optionally cancel after `cancelAfterMs`.
// Prints the terminal event and elapsed wall time.
void run(const char *name, long long timeoutMs, long long cancelAfterMs, const char *expect,
         const std::string &broker = "localhost:9092") {
  core::infra::KafkaSender sender;
  core::app::CancellationToken token;
  RecordingSink sink;
  auto model = makeConsumer(timeoutMs, broker);

  std::thread canceller;
  if (cancelAfterMs >= 0)
    canceller = std::thread([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(cancelAfterMs));
      token.cancel();
    });

  auto t0 = Clock::now();
  sender.execute(model, sink, token);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  if (canceller.joinable()) canceller.join();

  bool pass = sink.terminal == expect;
  std::printf("%-28s -> %-20s records=%d elapsed=%lldms  expect=%-20s [%s]%s%s\n", name,
              sink.terminal.c_str(), sink.records, (long long)ms, expect, pass ? "PASS" : "FAIL",
              sink.detail.empty() ? "" : "  msg=", sink.detail.c_str());
}

} // namespace

int main(int argc, char **argv) {
  const std::string phase = argc > 1 ? argv[1] : "";
  if (phase == "off") {
    // Broker DOWN. 1a: port closed -> refused -> immediate Network failure. 1b: blackhole (unroutable
    // IP, SYN never answered) -> connect hangs -> Network failure only after the full timeout.
    run("1a: broker refused", 2000, -1, "failed:network");
    run("1b: blackhole timeout", 2000, -1, "failed:network", "10.255.255.1:9092");
    // 2: cancel wins mid-connect (blackhole keeps the connect phase alive long enough to press Cancel).
    run("2: cancel while connecting", 10000, 500, "failed:cancelled", "10.255.255.1:9092");
  } else if (phase == "on") {
    // Broker UP.
    run("3: session timeout (live)", 3000, -1, "closed:timeout");
    run("4: cancel while consuming", 30000, 2000, "closed:cancelled");
  } else {
    std::printf("usage: kafka_manual off|on\n");
    return 2;
  }
  return 0;
}
