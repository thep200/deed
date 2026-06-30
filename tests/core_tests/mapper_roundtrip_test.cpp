// mapper_roundtrip_test.cpp — REFACTOR_SPEC §8.1/§11.2 acceptance gate.
// For each request type: build a domain RequestModel, serialize to the on-disk JSON schema, parse it back,
// and assert the domain value is recovered exactly (toJson(model) -> fromJson -> == model). Also verifies a
// hand-written HTTP JSON document (the real file schema) parses into the expected domain value.
#include <cstdio>
#include <string>
#include <vector>

#include "core/domain/request/request_model.hpp"
#include "infra/serialization/request_json_mapper.hpp"

using namespace core::domain;
using core::infra::RequestJsonMapper;

static int rt_pass = 0, rt_fail = 0;
#define RT_CHECK(cond, msg)                                                                        \
  do {                                                                                             \
    if (cond) { ++rt_pass; }                                                                       \
    else { ++rt_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }             \
  } while (0)

static RequestConfig cfg() { return RequestConfig{Timeout::fromMillis(5000).take(), true}; }

static void roundtrip(const RequestModel &model, const char *label) {
  RequestJsonMapper mapper;
  std::string json = mapper.toJson(model);
  auto back = mapper.fromJson(json);
  if (!back.isOk()) {
    ++rt_fail;
    std::printf("  FAIL: %s fromJson failed: %s\n", label, back.error().message.c_str());
    return;
  }
  RT_CHECK(back.value() == model, label);
}

static void test_http() {
  HttpRequest::Parts p{HttpMethod::Post, Url::create("https://api.example.com/users").take()};
  p.params = QueryParamList(std::vector<QueryParam>{QueryParam::create("page", "1").value()});
  p.headers = HeaderList(std::vector<Header>{Header::create("Content-Type", "application/json").value(),
                                             Header::create("X-Off", "x", false).value()});
  p.body = Body::raw(RawSubtype::Json, "{\"a\":1}");
  p.auth = Auth::bearer("tok123").value();
  auto m = RequestModel::create(RequestId("req_http_1"), "Create User", 0, cfg(),
                                HttpRequest::create(std::move(p)).take());
  roundtrip(m.value(), "http roundtrip");
}

static void test_http_form_body() {
  HttpRequest::Parts p{HttpMethod::Put, Url::create("https://api/x").take()};
  p.body = Body::formUrlEncoded({{"k1", "v1", true}, {"k2", "v2", false}});
  auto m = RequestModel::create(RequestId("req_http_2"), "Form", 1, cfg(),
                                HttpRequest::create(std::move(p)).take());
  roundtrip(m.value(), "http form-body roundtrip");
}

static void test_grpc() {
  GrpcRequest::Parts p;
  p.target = "localhost:50051";
  p.service = "calc.Calc";
  p.method = "Add";
  p.methodType = GrpcMethodType::ServerStreaming;
  p.message = JsonText::of("{\"a\":2}");
  p.metadata = GrpcMetadata::create({{"x-trace", "1", true}}).take();
  auto m = RequestModel::create(RequestId("req_grpc_1"), "Add", 2, cfg(),
                                GrpcRequest::create(std::move(p)).take());
  roundtrip(m.value(), "grpc roundtrip");
}

static void test_ws() {
  WebSocketRequest::Parts p{Url::create("wss://echo.example.com/socket").take()};
  p.subprotocols = {"graphql-ws"};
  p.onOpenSend = {WsMessage{WsSendKind::Text, "hello"}};
  auto m = RequestModel::create(RequestId("req_ws_1"), "Echo", 3, cfg(),
                                WebSocketRequest::create(std::move(p)).take());
  roundtrip(m.value(), "ws roundtrip");
}

static void test_graphql() {
  GraphQlRequest::Parts p{Url::create("https://api/graphql").take(), {}, {}, Auth::none(),
                          GqlSubTransport::Http, ""};
  p.op.query = "query Me { me { id } }";
  p.op.operationName = "Me";
  p.op.variables = JsonText::of("{\"x\":1}");
  p.op.operation = GqlOperationType::Query;
  auto m = RequestModel::create(RequestId("req_gql_1"), "Me", 4, cfg(),
                                GraphQlRequest::create(std::move(p)).take());
  roundtrip(m.value(), "graphql roundtrip");
}

static void test_parse_real_schema() {
  // The exact on-disk schema (envelope + http block). fromJson must yield the expected domain value.
  const std::string doc = R"({
    "schemaVersion": 1, "id": "req_x", "name": "Get", "type": "http", "seq": 0,
    "config": {"timeout_ms": 3000, "tls": true},
    "http": {"method": "GET", "url": "https://h/x", "params": [], "headers": [], "pathVariables": [],
             "body": {"mode": "none"}, "auth": {"type": "none"}}
  })";
  RequestJsonMapper mapper;
  auto r = mapper.fromJson(doc);
  RT_CHECK(r.isOk(), "real schema parses");
  if (r.isOk()) {
    const auto &m = r.value();
    RT_CHECK(m.id().get() == "req_x" && m.type() == RequestType::Http, "envelope fields parsed");
    RT_CHECK(m.config().timeout.millis() == 3000, "config timeout parsed");
  }
}

// On-disk-format compat: the native mapper must read EXISTING files written by the legacy json_codec —
// WS onOpenSend as a string[] (kind = defaultSendKind), gql subTransport "ws"/"sse" + wsProtocol alias.
static void test_parse_legacy_ws_gql() {
  RequestJsonMapper mapper;
  const std::string ws = R"({
    "id": "w", "name": "W", "type": "ws", "seq": 0, "config": {"timeout_ms": 5000, "tls": true},
    "ws": {"url": "wss://h/s", "headers": [], "subprotocols": ["graphql-ws"],
           "onOpenSend": ["hi","bye"], "defaultSendKind": "text", "auth": {"type":"none"}}
  })";
  auto rw = mapper.fromJson(ws);
  RT_CHECK(rw.isOk(), "legacy ws doc parses");
  if (rw.isOk()) {
    const auto &w = std::get<WebSocketRequest>(rw.value().payload());
    RT_CHECK(w.onOpenSend().size() == 2 && w.onOpenSend()[0].payload == "hi" &&
                 w.onOpenSend()[0].kind == WsSendKind::Text,
             "legacy ws onOpenSend string[] -> WsMessage{defaultSendKind,payload}");
  }
  const std::string gql = R"({
    "id": "g", "name": "G", "type": "graphql", "seq": 0, "config": {"timeout_ms": 5000, "tls": true},
    "graphql": {"url": "wss://h/gql", "query": "subscription { x }", "variables": "{}",
                "operationName": "", "operation": "subscription", "subTransport": "ws",
                "wsProtocol": "subscriptions-transport-ws", "headers": [], "auth": {"type":"none"}}
  })";
  auto rg = mapper.fromJson(gql);
  RT_CHECK(rg.isOk(), "legacy gql subscription doc parses");
  if (rg.isOk()) {
    const auto &g = std::get<GraphQlRequest>(rg.value().payload());
    RT_CHECK(g.subTransport() == GqlSubTransport::Ws && g.wsProtocol() == "graphql-ws",
             "legacy gql subTransport ws + wsProtocol alias -> domain");
  }
}

int run_mapper_roundtrip_tests() {
  std::printf("[mapper_roundtrip]\n");
  test_http();
  test_http_form_body();
  test_grpc();
  test_ws();
  test_graphql();
  test_parse_real_schema();
  test_parse_legacy_ws_gql();
  std::printf("  mapper: %d passed, %d failed\n", rt_pass, rt_fail);
  return rt_fail;
}
