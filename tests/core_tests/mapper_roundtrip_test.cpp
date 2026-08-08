#include <cstdio>
#include <string>
#include <vector>

#include "core/domain/kafka/kafka_request.hpp"
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

static void test_soap() {
  SoapRequest::Parts p{Url::create("http://svc.test/calc").take()};
  p.envelope = "<soapenv:Envelope><soapenv:Body/></soapenv:Envelope>";
  auto m1 = RequestModel::create(RequestId("req_soap_1"), "Calc", 9, cfg(),
                                 SoapRequest::create(std::move(p)).take());
  roundtrip(m1.value(), "soap 1.1 (empty action) roundtrip");

  SoapRequest::Parts p2{Url::create("https://svc.test/v2").take()};
  p2.action = "urn:GetUser";
  p2.version = SoapVersion::V1_2;
  p2.envelope = "<Envelope/>";
  std::vector<Header> hs;
  hs.push_back(Header::create("X-Trace", "1", true).take());
  p2.headers = HeaderList(std::move(hs));
  AuthOAuth2 o;
  o.tokenUrl = "https://idp/token"; o.clientId = "cid";
  p2.auth = Auth::oauth2(std::move(o)).take();
  auto m2 = RequestModel::create(RequestId("req_soap_2"), "GetUser", 10, cfg(),
                                 SoapRequest::create(std::move(p2)).take());
  roundtrip(m2.value(), "soap 1.2 + oauth2 + headers roundtrip");
}

static void test_ldap() {
  LdapRequest::Parts p{Url::create("ldap://dir.test:389").take()};
  auto m1 = RequestModel::create(RequestId("req_ldap_1"), "Dir", 11, cfg(),
                                 LdapRequest::create(std::move(p)).take());
  roundtrip(m1.value(), "ldap defaults roundtrip");

  LdapRequest::Parts p2{Url::create("ldaps://dir.test:636").take()};
  p2.bindDn = "cn=svc,dc=x";
  p2.bindPassword = "pw";
  p2.baseDn = "ou=people,dc=x";
  p2.scope = LdapScope::One;
  p2.filter = "(uid={{user}})";
  p2.attributes = {"cn", "mail", "memberOf"};
  p2.group = "cn=admins,dc=x";
  p2.testPassword = "{{user_pw}}";
  p2.sizeLimit = 5;
  p2.timeLimit = 2;
  p2.pageSize = 250;
  auto m2 = RequestModel::create(RequestId("req_ldap_2"), "Check User", 12, cfg(),
                                 LdapRequest::create(std::move(p2)).take());
  roundtrip(m2.value(), "ldap full (group + bind-test) roundtrip");
}

static void test_kafka_producer() {
  auto brokers = BrokerList::parse("localhost:9092").take();
  KafkaProduceConfig pcfg{KafkaTopic::create("demo-topic").take()};
  pcfg.acks = Acks::All;
  pcfg.compression = Compression::None;
  pcfg.valueFormat = KafkaValueFormat::Avro; // Avro fields must survive the file round-trip
  pcfg.schemaRegistry = {"http://localhost:8081", "sr-user", "sr-pass"};
  KafkaMessage msg;
  msg.value = MessagePayload{"{\n  \"hello\": \"world\"\n}"};
  auto req =
      KafkaRequest::create(brokers, KafkaSecurity::plaintext(), KafkaRequest::Mode{KafkaProduceSpec{pcfg, msg}})
          .take();
  auto m = RequestModel::create(RequestId("req_kafka_p1"), "Produce", 5, cfg(), req);
  roundtrip(m.value(), "kafka producer roundtrip");
}

static void test_kafka_consumer() {
  auto brokers = BrokerList::parse("localhost:9092").take();
  KafkaConsumeConfig ccfg{{KafkaTopic::create("demo-topic").take()}, std::nullopt,
                          ConsumerGroup::create("deed-tail-local").take()};
  ccfg.schemaRegistry = {"http://localhost:8081", "", ""};
  auto req = KafkaRequest::create(brokers, KafkaSecurity::plaintext(), KafkaRequest::Mode{KafkaConsumeSpec{ccfg}})
                 .take();
  auto m = RequestModel::create(RequestId("req_kafka_c1"), "Consume", 6, cfg(), req);
  roundtrip(m.value(), "kafka consumer roundtrip");
}

// Both sides persist: the inactive kind rides along as inactiveDraft — toggling Producer/Consumer then quitting must not lose the other side.
static void test_kafka_inactive_draft() {
  auto brokers = BrokerList::parse("localhost:9092").take();
  KafkaProduceConfig pcfg{KafkaTopic::create("produce-topic").take()};
  pcfg.valueFormat = KafkaValueFormat::Avro;
  pcfg.schemaRegistry = {"http://localhost:8081", "", ""};
  KafkaMessage msg;
  msg.value = MessagePayload{"{\n  \"draft\": true\n}"};
  KafkaConsumeConfig ccfg{{KafkaTopic::create("consume-topic").take()}, std::nullopt,
                          ConsumerGroup::create("deed-tail-draft").take()};

  auto producerActive = KafkaRequest::create(brokers, KafkaSecurity::plaintext(),
                                             KafkaRequest::Mode{KafkaProduceSpec{pcfg, msg}},
                                             KafkaRequest::Mode{KafkaConsumeSpec{ccfg}})
                            .take();
  auto m1 = RequestModel::create(RequestId("req_kafka_d1"), "ProduceDraft", 7, cfg(), producerActive);
  roundtrip(m1.value(), "kafka producer-active + consumer-draft roundtrip");

  auto consumerActive = KafkaRequest::create(brokers, KafkaSecurity::plaintext(),
                                             KafkaRequest::Mode{KafkaConsumeSpec{ccfg}},
                                             KafkaRequest::Mode{KafkaProduceSpec{pcfg, msg}})
                            .take();
  auto m2 = RequestModel::create(RequestId("req_kafka_d2"), "ConsumeDraft", 8, cfg(), consumerActive);
  roundtrip(m2.value(), "kafka consumer-active + producer-draft roundtrip");

  RT_CHECK(!KafkaRequest::create(brokers, KafkaSecurity::plaintext(),
                                 KafkaRequest::Mode{KafkaConsumeSpec{ccfg}},
                                 KafkaRequest::Mode{KafkaConsumeSpec{ccfg}})
                .isOk(),
           "kafka same-kind inactiveDraft rejected");
}

// The spec's exact sample JSON, with timeout_ms adjusted to a positive value (0 is invalid per Timeout::fromMillis).
static void test_parse_kafka_samples() {
  RequestJsonMapper mapper;
  const std::string producer = R"({
    "config": { "timeout_ms": 1800000, "tls": false },
    "id": "k1example0001",
    "kafka": {
      "brokers": "localhost:9092",
      "clientKind": "producer",
      "consumer": null,
      "producer": {
        "config": {
          "acks": "all", "clientId": "deed", "compression": "none", "extra": [],
          "idempotence": false, "lingerMs": 0, "messageTimeoutMs": 30000,
          "partition": -1, "retries": 3, "topic": "demo-topic"
        },
        "message": {
          "headers": [], "key": "",
          "value": "{\n  \"hello\": \"world\"\n}"
        }
      },
      "security": { "type": "plaintext" }
    },
    "name": "Produce - local",
    "schemaVersion": 1, "seq": 0, "type": "kafka"
  })";
  auto rp = mapper.fromJson(producer);
  RT_CHECK(rp.isOk(), "kafka producer sample parses");
  if (rp.isOk()) {
    const auto &k = std::get<KafkaRequest>(rp.value().payload());
    RT_CHECK(rp.value().type() == RequestType::Kafka, "sample type == Kafka");
    RT_CHECK(k.kind() == KafkaClientKind::Producer, "sample clientKind producer");
    RT_CHECK(k.brokers().toBootstrapServers() == "localhost:9092", "sample brokers");
    const auto &p = std::get<KafkaProduceSpec>(k.mode());
    RT_CHECK(p.config.topic.value() == "demo-topic", "sample topic");
    RT_CHECK(p.config.partition.value == KafkaPartition::kAuto, "sample partition auto");
    RT_CHECK(p.message.value.value == "{\n  \"hello\": \"world\"\n}", "sample value (always JSON, no format field)");
  }

  const std::string consumer = R"({
    "config": { "timeout_ms": 1800000, "tls": false },
    "id": "kc1example001",
    "kafka": {
      "brokers": "localhost:9092",
      "clientKind": "consumer",
      "consumer": {
        "config": {
          "autoCommit": true, "clientId": "deed", "extra": [], "group": "deed-tail-local",
          "maxMessages": null, "offsetReset": "latest", "partition": -1,
          "pollTimeoutMs": 500, "topics": ["demo-topic"]
        }
      },
      "producer": null,
      "security": { "type": "plaintext" }
    },
    "name": "Consume - local",
    "schemaVersion": 1, "seq": 0, "type": "kafka"
  })";
  auto rc = mapper.fromJson(consumer);
  RT_CHECK(rc.isOk(), "kafka consumer sample parses");
  if (rc.isOk()) {
    const auto &k = std::get<KafkaRequest>(rc.value().payload());
    RT_CHECK(k.kind() == KafkaClientKind::Consumer, "sample clientKind consumer");
    const auto &c = std::get<KafkaConsumeSpec>(k.mode());
    RT_CHECK(c.config.topics.size() == 1 && c.config.topics[0].value() == "demo-topic", "sample topics");
    RT_CHECK(!c.config.partition.has_value(), "sample partition -1 -> nullopt (subscribe)");
    RT_CHECK(!c.config.maxMessages.has_value(), "sample maxMessages null -> nullopt");
    RT_CHECK(c.config.group.value() == "deed-tail-local", "sample group");
  }

  if (rp.isOk()) roundtrip(rp.value(), "kafka producer sample re-roundtrip");
  if (rc.isOk()) roundtrip(rc.value(), "kafka consumer sample re-roundtrip");
}

static void test_parse_real_schema() {
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

// Back-compat: files written by the legacy codec must still parse (WS onOpenSend string[], gql wsProtocol alias).
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

// A present-but-unknown "type" must be a hard parse error (used to silently become empty HTTP).
static void test_unknown_type_rejected() {
  RequestJsonMapper mapper;
  const std::string doc = R"({
    "id": "u", "name": "U", "type": "mqtt", "seq": 0,
    "config": {"timeout_ms": 3000, "tls": true}
  })";
  auto r = mapper.fromJson(doc);
  RT_CHECK(!r.isOk(), "unknown type token -> parse error");
  if (!r.isOk())
    RT_CHECK(r.error().message.find("mqtt") != std::string::npos, "error names the bad token");
}

// Legacy files may lack "type" entirely -> must keep parsing as HTTP.
static void test_missing_type_defaults_http() {
  RequestJsonMapper mapper;
  const std::string doc = R"({
    "id": "l", "name": "Legacy", "seq": 0, "config": {"timeout_ms": 3000, "tls": true},
    "http": {"method": "GET", "url": "https://h/x", "params": [], "headers": [], "pathVariables": [],
             "body": {"mode": "none"}, "auth": {"type": "none"}}
  })";
  auto r = mapper.fromJson(doc);
  RT_CHECK(r.isOk() && r.value().type() == RequestType::Http, "missing type field -> http");
}

int run_mapper_roundtrip_tests() {
  std::printf("[mapper_roundtrip]\n");
  test_http();
  test_http_form_body();
  test_grpc();
  test_ws();
  test_graphql();
  test_soap();
  test_ldap();
  test_kafka_producer();
  test_kafka_consumer();
  test_kafka_inactive_draft();
  test_parse_kafka_samples();
  test_parse_real_schema();
  test_parse_legacy_ws_gql();
  test_missing_type_defaults_http();
  test_unknown_type_rejected();
  std::printf("  mapper: %d passed, %d failed\n", rt_pass, rt_fail);
  return rt_fail;
}
