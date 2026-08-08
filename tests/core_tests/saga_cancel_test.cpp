#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/app/cancellation_token.hpp"
#include "core/app/request_orchestrator.hpp"
#include "core/app/send_request_saga.hpp"
#include "infra/transport/http/native_http_sender.hpp"
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

// Cancel must reach a sender already blocked in execute(): token polling cannot unpark a syscall, so senders register an abort hook.
static void test_cancel_hook_unblocks_sender() {
  struct BlockingSender final : IRequestSender {
    std::atomic<bool> entered{false};
    std::atomic<bool> aborted{false};
    bool supports(RequestType t) const override { return t == RequestType::Http; }
    Status execute(const RequestModel &, IResponseSink &sink, const ICancellationToken &cancel) override {
      // Same shape as the real senders: hand the transport an abort hook, then block.
      cancel.onCancel([this] { aborted.store(true); });
      entered.store(true);
      while (!aborted.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      sink.emit(ResponseEvent(EvFailed{{ErrorKind::Cancelled, "Cancelled", {}}}));
      return core::domain::ok();
    }
  } sender;

  FakeClock clock;
  SendRequestSaga saga(RequestExecutionId("e20"), makeHttp(), {{&sender}, &clock, nullptr, nullptr});
  FakeObserver obs;
  std::thread worker([&] { saga.run(obs); });
  while (!sender.entered.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  saga.cancel();
  worker.join();

  SG_CHECK(sender.aborted.load(), "cancel hook: sender abort hook fired mid-execute");
  SG_CHECK(saga.state() == SagaState::Cancelled, "cancel hook: state Cancelled");
}

// A hook registered after the token already tripped must run immediately (the senders' pre-flight check).
static void test_cancel_hook_runs_when_already_cancelled() {
  core::app::CancellationToken tok;
  tok.cancel();
  bool ran = false;
  tok.onCancel([&] { ran = true; });
  SG_CHECK(ran, "cancel hook: late registration fires at once");

  int count = 0;
  core::app::CancellationToken t2;
  t2.onCancel([&] { ++count; });
  t2.cancel();
  t2.cancel(); // idempotent
  SG_CHECK(count == 1, "cancel hook: fires exactly once");
}

// The UI's escalation step: no exec handle (or the wrong one) must not leave a hung send running.
static void test_orchestrator_cancel_all() {
  struct BlockingSender final : IRequestSender {
    std::atomic<int> live{0};
    bool supports(RequestType t) const override { return t == RequestType::Http; }
    Status execute(const RequestModel &, IResponseSink &sink, const ICancellationToken &cancel) override {
      live.fetch_add(1);
      while (!cancel.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      live.fetch_sub(1);
      sink.emit(ResponseEvent(EvFailed{{ErrorKind::Cancelled, "Cancelled", {}}}));
      return core::domain::ok();
    }
  } sender;

  FakeClock clock;
  std::vector<std::thread> threads;
  RequestOrchestrator orch({{&sender}, &clock, nullptr, nullptr},
                           [&threads](std::function<void()> job) { threads.emplace_back(std::move(job)); },
                           [&threads](std::function<void()> job) { threads.emplace_back(std::move(job)); });
  auto o1 = std::make_shared<FakeObserver>();
  auto o2 = std::make_shared<FakeObserver>();
  orch.send(makeHttp(), o1);
  orch.send(makeHttp(), o2);
  while (sender.live.load() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  orch.cancelAll();
  for (auto &t : threads) t.join();
  SG_CHECK(sender.live.load() == 0, "cancelAll: every in-flight saga stopped");
  SG_CHECK(!o1->events.empty() && o1->events.back().is<EvFailed>(), "cancelAll: first send settled");
  SG_CHECK(!o2->events.empty() && o2->events.back().is<EvFailed>(), "cancelAll: second send settled");
}

// Senders are shared singletons: cancelling one send must not touch a concurrent send on the same instance.
static void test_cancel_isolated_between_concurrent_sends() {
  struct SharedSender final : IRequestSender {
    std::atomic<int> live{0};
    bool supports(RequestType t) const override { return t == RequestType::Http; }
    Status execute(const RequestModel &, IResponseSink &sink, const ICancellationToken &cancel) override {
      live.fetch_add(1);
      while (!cancel.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      live.fetch_sub(1);
      sink.emit(ResponseEvent(EvFailed{{ErrorKind::Cancelled, "Cancelled", {}}}));
      return core::domain::ok();
    }
  } sender;

  FakeClock clock;
  std::vector<std::thread> threads;
  RequestOrchestrator orch({{&sender}, &clock, nullptr, nullptr},
                           [&threads](std::function<void()> job) { threads.emplace_back(std::move(job)); },
                           [&threads](std::function<void()> job) { threads.emplace_back(std::move(job)); });
  auto o1 = std::make_shared<FakeObserver>();
  auto o2 = std::make_shared<FakeObserver>();
  auto e1 = orch.send(makeHttp(), o1);
  auto e2 = orch.send(makeHttp(), o2);
  while (sender.live.load() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  orch.cancel(e1.value());
  for (int i = 0; i < 500 && sender.live.load() != 1; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  SG_CHECK(sender.live.load() == 1, "cancel isolation: only the targeted send stopped");
  SG_CHECK(!o2->events.back().is<EvFailed>(), "cancel isolation: the other send did not settle");

  orch.cancel(e2.value());
  for (auto &t : threads) t.join();
  SG_CHECK(sender.live.load() == 0, "cancel isolation: second send still cancellable afterwards");
}

// A real HTTP send parked on a blackholed address must stop when Cancel trips, not at the request timeout; a fast reject still passes.
static void test_http_cancel_stops_hung_connect() {
  HttpRequest::Parts p{HttpMethod::Get, Url::create("http://10.255.255.1:81/hang").take()};
  RequestConfig cfg{Timeout::fromMillis(60000).take(), true}; // 60s: only Cancel can end this in time
  RequestModel model =
      RequestModel::create(RequestId("req_hang"), "H", 0, cfg, HttpRequest::create(std::move(p)).take())
          .take();

  core::app::CancellationToken token;
  struct CountingSink final : IResponseSink {
    int terminals = 0;
    void emit(const ResponseEvent &ev) override {
      if (ev.is<EvCompleted>() || ev.is<EvFailed>()) ++terminals;
    }
  } sink;

  core::infra::NativeHttpSender http;
  const auto t0 = std::chrono::steady_clock::now();
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    token.cancel();
  });
  http.execute(model, sink, token);
  canceller.join();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();

  SG_CHECK(ms < 10000, "http cancel: hung connect released well before the 60s timeout");
  SG_CHECK(sink.terminals == 1, "http cancel: exactly one terminal event");
}

int run_saga_cancel_tests() {
  std::printf("[saga_cancel]\n");
  test_cancel_hook_unblocks_sender();
  test_cancel_hook_runs_when_already_cancelled();
  test_orchestrator_cancel_all();
  test_cancel_isolated_between_concurrent_sends();
  test_http_cancel_stops_hung_connect();
  std::printf("  saga_cancel: %d passed, %d failed\n", sg_pass, sg_fail);
  return sg_fail;
}
