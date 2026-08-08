// e2e against the local WS echo server; base ws URL is argv[1] (e.g. ws://127.0.0.1:PORT).
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

// Frames arrive as JSON envelopes {"dir":"in|out",...,"data":"..."}; assertions match INBOUND (dir:in) echoes.
struct Collector final : IRequestObserver {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::string> messages;
  bool closed = false;
  void onEvent(RequestExecutionId, const ResponseEvent &ev) noexcept override {
    std::lock_guard<std::mutex> lk(mu);
    if (const auto *m = ev.get<EvMessage>()) messages.push_back(m->payload);
    if (ev.is<EvClosed>() || ev.is<EvFailed>()) closed = true;
    cv.notify_all();
  }
  bool hasInboundLocked(const std::string &needle) const {
    for (const auto &m : messages)
      if (m.find("\"dir\":\"in\"") != std::string::npos && m.find(needle) != std::string::npos)
        return true;
    return false;
  }
  bool waitInbound(const std::string &needle, int ms = 5000) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return hasInboundLocked(needle); });
  }
  bool waitClosed(int ms = 5000) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return closed; });
  }
};
} // namespace

int main(int argc, char **argv) {
  std::string url = argc > 1 ? argv[1] : "ws://127.0.0.1:18090";
  std::printf("== ws_e2e (url=%s) ==\n", url.c_str());

  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto root = fs::temp_directory_path() / ("deed_ws_e2e_" + std::to_string(stamp));
  fs::create_directories(root);
  core::app::CoreApiClient::Config cfg;
  cfg.collectionRoot = root.string();
  auto client = core::app::CoreApiClient::create(std::move(cfg));

  WebSocketRequest::Parts p{Url::createWithSchemes(url, {"ws", "wss"}).take()};
  p.onOpenSend = {WsMessage{WsSendKind::Text, "ping"}};
  RequestConfig rc{Timeout::fromMillis(15000).take(), true};
  auto req = RequestModel::create(RequestId("ws1"), "ws", 0, rc,
                                  WebSocketRequest::create(std::move(p)).take())
                 .take();

  auto obs = std::make_shared<Collector>();
  auto exec = client->send(req, obs);
  check(exec.isOk(), "WS send accepted");

  // 1. onOpenSend "ping" echoed back as an inbound frame.
  check(obs->waitInbound("\"data\":\"ping\""), "WS received echoed onOpen frame (ping)");

  // 2. push a frame mid-session -> echoed back inbound.
  if (exec.isOk()) {
    auto s = client->sendStreamMessage(exec.value(), WsMessage{WsSendKind::Text, "hello"});
    check(s.isOk(), "WS push accepted");
    check(obs->waitInbound("\"data\":\"hello\""), "WS received echoed pushed frame (hello)");
  }

  // 3. close the session -> EvClosed terminal.
  if (exec.isOk()) {
    client->closeStream(exec.value(), 1000, "bye");
    check(obs->waitClosed(), "WS closed -> terminal event");
  }

  // 4. Handshake auth: a Bearer token resolved from {{var}} is applied to the handshake; the server echoes Authorization back.
  {
    VariableScope scope;
    scope.values["tok"] = "secret-w";
    client->setVariableScope(scope);
    WebSocketRequest::Parts ap{Url::createWithSchemes(url, {"ws", "wss"}).take()};
    ap.auth = Auth::bearer("{{tok}}").take();
    RequestConfig arc{Timeout::fromMillis(15000).take(), true};
    auto areq = RequestModel::create(RequestId("wsauth"), "wsauth", 0, arc,
                                     WebSocketRequest::create(std::move(ap)).take())
                    .take();
    auto aobs = std::make_shared<Collector>();
    auto aexec = client->send(areq, aobs);
    check(aexec.isOk(), "WS auth send accepted");
    check(aobs->waitInbound("Bearer secret-w"), "WS handshake applied resolved Bearer auth");
    if (aexec.isOk()) client->closeStream(aexec.value(), 1000, "bye");
    client->setVariableScope({});
  }

  fs::remove_all(root);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
