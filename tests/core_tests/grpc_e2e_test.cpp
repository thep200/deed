// grpc_e2e_test.cpp — REFACTOR_SPEC e2e for the gRPC path through the new domain stack: CoreApiClient ->
// RequestOrchestrator -> SendRequestSaga -> LegacySenderAdapter(GrpcSender) -> grpc++ -> the Go reflection
// echo server (tests/grpcserver). Covers server reflection (listGrpcMethods), a unary call, and a
// server-streaming call. argv[1] is "grpc://host:port" (run_e2e strips nothing; we take host:port).
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/app/core_api_client.hpp"
#include "core/domain/request/request_model.hpp"

namespace fs = std::filesystem;
using namespace core::domain;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char *msg) {
  if (ok) ++g_pass;
  else { ++g_fail; std::printf("  FAIL: %s\n", msg); }
}

struct Collector final : IRequestObserver {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<ResponseEvent> events;
  bool terminal = false;
  void onEvent(RequestExecutionId, const ResponseEvent &ev) noexcept override {
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(ev);
    if (ev.isTerminal()) { terminal = true; cv.notify_all(); }
  }
  bool waitTerminal(int ms = 8000) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return terminal; });
  }
  const EvCompleted *completed() const {
    for (const auto &e : events)
      if (const auto *c = e.get<EvCompleted>()) return c;
    return nullptr;
  }
  const EvFailed *failed() const {
    for (const auto &e : events)
      if (const auto *f = e.get<EvFailed>()) return f;
    return nullptr;
  }
  size_t messageCount() const {
    size_t n = 0;
    for (const auto &e : events)
      if (e.get<EvMessage>()) ++n;
    return n;
  }
  std::string concatMessages() const {
    std::string s;
    for (const auto &e : events)
      if (const auto *m = e.get<EvMessage>()) s += m->payload;
    return s;
  }
};

RequestModel grpcReq(const std::string &target, const std::string &service, const std::string &method,
                     GrpcMethodType mt, const std::string &messageJson) {
  GrpcRequest::Parts gp;
  gp.target = target;
  gp.service = service;
  gp.method = method;
  gp.methodType = mt;
  gp.message = JsonText::of(messageJson);
  // protoSource defaults to reflection, metadata empty.
  RequestConfig cfg{Timeout::fromMillis(15000).take(), false}; // plaintext (no TLS)
  return RequestModel::create(RequestId("g"), "g", 0, cfg, GrpcRequest::create(std::move(gp)).take()).take();
}
} // namespace

int main(int argc, char **argv) {
  std::string arg = argc > 1 ? argv[1] : "grpc://127.0.0.1:18070";
  std::string target = arg;
  auto pos = target.find("://");
  if (pos != std::string::npos) target = target.substr(pos + 3); // strip scheme -> host:port
  std::printf("== grpc_e2e (target=%s) ==\n", target.c_str());

  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto root = fs::temp_directory_path() / ("deed_grpc_e2e_" + std::to_string(stamp));
  fs::create_directories(root);
  core::app::CoreApiClient::Config ccfg;
  ccfg.collectionRoot = root.string();
  auto client = core::app::CoreApiClient::create(std::move(ccfg));

  // 1. Server reflection: list the echo service's methods.
  {
    GrpcRequest::Parts gp;
    gp.target = target;
    auto greq = GrpcRequest::create(std::move(gp)).take();
    auto methods = client->listGrpcMethods(greq);
    check(methods.isOk(), "reflection listGrpcMethods ok");
    if (methods.isOk()) {
      bool unary = false, stream = false;
      for (const auto &m : methods.value()) {
        if (m.service == "echo.Echo" && m.method == "Unary") unary = true;
        if (m.service == "echo.Echo" && m.method == "ServerStream") stream = true;
      }
      check(unary, "reflection found echo.Echo/Unary");
      check(stream, "reflection found echo.Echo/ServerStream");
    }
  }

  // 2. Unary call -> EvCompleted carrying the echoed message.
  {
    auto obs = std::make_shared<Collector>();
    auto req = grpcReq(target, "echo.Echo", "Unary", GrpcMethodType::Unary, "{\"msg\":\"hi-unary\"}");
    client->send(req, obs);
    obs->waitTerminal();
    const auto *c = obs->completed();
    check(c != nullptr && !obs->failed(), "gRPC unary completed (not failed)");
    if (c) check(c->summary.body.find("hi-unary") != std::string::npos, "gRPC unary echoed message");
  }

  // 3. Server-streaming call (count=3) -> 3 inbound messages.
  {
    auto obs = std::make_shared<Collector>();
    auto req = grpcReq(target, "echo.Echo", "ServerStream", GrpcMethodType::ServerStreaming,
                       "{\"msg\":\"s\",\"count\":3}");
    client->send(req, obs);
    obs->waitTerminal();
    check(obs->messageCount() == 3, "gRPC server-stream received 3 messages");
    check(obs->concatMessages().find("s#2") != std::string::npos, "gRPC server-stream last echo present");
  }

  // 4. Client-streaming call: send 3 request messages (JSON array) -> ONE response joining them with ",".
  {
    auto obs = std::make_shared<Collector>();
    auto req = grpcReq(target, "echo.Echo", "ClientStream", GrpcMethodType::ClientStreaming,
                       "[{\"msg\":\"a\"},{\"msg\":\"b\"},{\"msg\":\"c\"}]");
    client->send(req, obs);
    obs->waitTerminal();
    const auto *c = obs->completed();
    check(c != nullptr && !obs->failed(), "gRPC client-stream completed (not failed)");
    if (c) check(c->summary.body.find("a,b,c") != std::string::npos, "gRPC client-stream joined messages");
  }

  // 5. Bidi-streaming call: send 3 request messages (JSON array) -> 3 echoed inbound messages.
  {
    auto obs = std::make_shared<Collector>();
    auto req = grpcReq(target, "echo.Echo", "BiDi", GrpcMethodType::BidiStreaming,
                       "[{\"msg\":\"x\"},{\"msg\":\"y\"},{\"msg\":\"z\"}]");
    client->send(req, obs);
    obs->waitTerminal();
    check(obs->messageCount() == 3, "gRPC bidi received 3 messages");
    const std::string all = obs->concatMessages();
    check(all.find("x") != std::string::npos && all.find("z") != std::string::npos,
          "gRPC bidi echoed all messages");
  }

  fs::remove_all(root);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
