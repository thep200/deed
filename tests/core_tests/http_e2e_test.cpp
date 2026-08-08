// e2e against the Go test server (tests/testserver) — no external network; base URL is argv[1].
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/app/core_api_client.hpp"
#include "core/domain/request/request_model.hpp"

using namespace core::domain;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char *msg) {
  if (ok) ++g_pass;
  else { ++g_fail; std::printf("  FAIL: %s\n", msg); }
}

// Collects the saga's events (they arrive on a pool thread); waitTerminal() blocks until a terminal event or timeout.
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
  bool waitTerminal(int ms = 5000) {
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
  size_t messageCount() {
    std::lock_guard<std::mutex> lk(mu);
    size_t n = 0;
    for (const auto &e : events)
      if (e.get<EvMessage>()) ++n;
    return n;
  }
  std::string concatMessages() {
    std::lock_guard<std::mutex> lk(mu);
    std::string s;
    for (const auto &e : events)
      if (const auto *m = e.get<EvMessage>()) s += m->payload;
    return s;
  }
};

RequestModel httpReq(HttpMethod m, const std::string &url, Body body, std::vector<Header> headers) {
  HttpRequest::Parts p{m, Url::create(url).take()};
  p.body = std::move(body);
  p.headers = HeaderList(std::move(headers));
  RequestConfig cfg{Timeout::fromMillis(10000).take(), true};
  return RequestModel::create(RequestId("e2e"), "e2e", 0, cfg, HttpRequest::create(std::move(p)).take())
      .take();
}
} // namespace

int main(int argc, char **argv) {
  std::string base = argc > 1 ? argv[1] : "http://127.0.0.1:18080";
  std::printf("== http_e2e (base=%s) ==\n", base.c_str());
  auto client = core::app::CoreApiClient::create();

  // 1. GET with a custom header echoed back.
  {
    auto obs = std::make_shared<Collector>();
    auto req = httpReq(HttpMethod::Get, base + "/hello", Body::none(),
                       {Header::create("X-Test", "abc").value()});
    auto exec = client->send(req, obs);
    check(exec.isOk(), "GET send accepted");
    obs->waitTerminal();
    const auto *c = obs->completed();
    check(c != nullptr, "GET completed event received");
    if (c) {
      check(c->summary.statusCode == 200, "GET status 200");
      check(c->summary.body.find("GET") != std::string::npos, "GET echoed method");
      check(c->summary.body.find("abc") != std::string::npos, "GET echoed header");
      check(c->summary.body.find("/hello") != std::string::npos, "GET echoed path");
    }
  }

  // 2. POST with a JSON body echoed back.
  {
    auto obs = std::make_shared<Collector>();
    auto req = httpReq(HttpMethod::Post, base + "/submit",
                       Body::raw(RawSubtype::Json, "{\"k\":42}"), {});
    auto exec = client->send(req, obs);
    check(exec.isOk(), "POST send accepted");
    obs->waitTerminal();
    const auto *c = obs->completed();
    check(c != nullptr && !obs->failed(), "POST completed (not failed)");
    if (c) {
      check(c->summary.statusCode == 200, "POST status 200");
      check(c->summary.body.find("42") != std::string::npos, "POST echoed json body");
    }
  }

  // 3. {{var}} resolution at the send boundary: a URL that is entirely a placeholder must reach the resolved host.
  {
    VariableScope scope;
    scope.values["base"] = base;
    client->setVariableScope(scope);
    auto obs = std::make_shared<Collector>();
    auto req = httpReq(HttpMethod::Get, "{{base}}/var", Body::none(), {});
    client->send(req, obs);
    obs->waitTerminal();
    const auto *c = obs->completed();
    check(c != nullptr && c->summary.statusCode == 200, "{{var}} resolved -> request reached host");
    if (c) check(c->summary.body.find("/var") != std::string::npos, "{{var}} resolved path");
    client->setVariableScope({}); // reset for later cases
  }

  // 3b. GraphQL over HTTP (GraphQlSender POSTs {query,variables} to /graphql).
  {
    GraphQlRequest::Parts gp{Url::create(base + "/graphql").take(), {}, {}, Auth::none(),
                             GqlSubTransport::Http, ""};
    gp.op.query = "query Me { me { id } }";
    gp.op.variables = JsonText::of("{\"x\":1}");
    RequestConfig cfg{Timeout::fromMillis(10000).take(), true};
    auto req = RequestModel::create(RequestId("gql"), "gql", 0, cfg,
                                    GraphQlRequest::create(std::move(gp)).take())
                   .take();
    auto obs = std::make_shared<Collector>();
    client->send(req, obs);
    obs->waitTerminal();
    const auto *c = obs->completed();
    check(c != nullptr && c->summary.statusCode == 200, "GraphQL completed 200");
    if (c) check(c->summary.body.find("echo") != std::string::npos, "GraphQL response echoed query");
  }

  // 3d. Native HTTP sender applies Bearer auth and resolves {{var}} inside the token (server echoes Authorization back).
  {
    VariableScope scope;
    scope.values["tok"] = "secret-h";
    client->setVariableScope(scope);
    HttpRequest::Parts p{HttpMethod::Get, Url::create(base + "/auth").take()};
    p.auth = Auth::bearer("{{tok}}").take();
    RequestConfig cfg{Timeout::fromMillis(10000).take(), true};
    auto req = RequestModel::create(RequestId("auth"), "auth", 0, cfg,
                                    HttpRequest::create(std::move(p)).take())
                   .take();
    auto obs = std::make_shared<Collector>();
    client->send(req, obs);
    obs->waitTerminal();
    const auto *c = obs->completed();
    check(c && c->summary.body.find("Bearer secret-h") != std::string::npos,
          "HTTP Bearer auth applied + {{var}} resolved");
    client->setVariableScope({});
  }

  // 3e. GraphQL sender applies auth with {{var}} in the token resolved (regression: literal "Bearer {{tok}}" reached the server).
  {
    VariableScope scope;
    scope.values["tok"] = "secret-g";
    client->setVariableScope(scope);
    GraphQlRequest::Parts gp{Url::create(base + "/graphql").take(), {}, {},
                             Auth::bearer("{{tok}}").take(), GqlSubTransport::Http, ""};
    gp.op.query = "query { x }";
    RequestConfig cfg{Timeout::fromMillis(10000).take(), true};
    auto req = RequestModel::create(RequestId("gqlauth"), "gqlauth", 0, cfg,
                                    GraphQlRequest::create(std::move(gp)).take())
                   .take();
    auto obs = std::make_shared<Collector>();
    client->send(req, obs);
    obs->waitTerminal();
    const auto *c = obs->completed();
    check(c && c->summary.body.find("Bearer secret-g") != std::string::npos,
          "GraphQL auth applied + {{var}} resolved by domain resolver");
    client->setVariableScope({});
  }

  // 3f. SSE: receive N events as EvMessage, then cancel (a real SSE client reconnects on clean close, so no natural terminal).
  {
    auto obs = std::make_shared<Collector>();
    auto req = httpReq(HttpMethod::Get, base + "/sse?count=3", Body::none(),
                       {Header::create("Accept", "text/event-stream").value()});
    auto exec = client->send(req, obs);
    check(exec.isOk(), "SSE send accepted");
    for (int i = 0; i < 100 && obs->messageCount() < 3; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(obs->messageCount() >= 3, "SSE received >=3 events");
    check(obs->concatMessages().find("msg#") != std::string::npos, "SSE event payload present");
    if (exec.isOk()) client->cancel(exec.value());
    obs->waitTerminal();
    check(obs->failed() != nullptr || obs->completed() != nullptr, "SSE reached a terminal after cancel");
  }

  // 3g. GraphQL subscription over WebSocket (graphql-transport-ws): 3 "next" events then "complete".
  {
    std::string wsBase = base;
    auto p = wsBase.find("://");
    if (p != std::string::npos) wsBase = "ws" + wsBase.substr(p);
    GraphQlRequest::Parts gp{Url::create(wsBase + "/graphqlws").take(), {}, {}, Auth::none(),
                             GqlSubTransport::Ws, "graphql-transport-ws"};
    gp.op.query = "subscription { n }";
    gp.op.operation = GqlOperationType::Subscription;
    RequestConfig cfg{Timeout::fromMillis(10000).take(), true};
    auto req = RequestModel::create(RequestId("sub"), "sub", 0, cfg,
                                    GraphQlRequest::create(std::move(gp)).take())
                   .take();
    auto obs = std::make_shared<Collector>();
    client->send(req, obs);
    obs->waitTerminal();
    check(obs->messageCount() == 3, "GraphQL subscription received 3 next events");
    check(obs->completed() != nullptr && !obs->failed(), "GraphQL subscription completed");
  }

  // 4. Connection failure surfaces as EvFailed (no server on this port).
  {
    auto obs = std::make_shared<Collector>();
    auto req = httpReq(HttpMethod::Get, "http://127.0.0.1:1/nope", Body::none(), {});
    client->send(req, obs);
    obs->waitTerminal();
    check(obs->failed() != nullptr, "unreachable host -> EvFailed");
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
