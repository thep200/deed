#include "infra/serialization/request_json_mapper.hpp"

#include <nlohmann/json.hpp>

#include "infra/serialization/json_codec.hpp"               // core::codec::parseGuarded (H5 depth guard) + safe getters
#include "core/infra/serialization/field_json.hpp"  // core::serial — domain field codecs (headers/auth/body/...)
#include "infra/serialization/wire_format.hpp"      // core::wire::* on-disk request type tokens

namespace core::infra {
namespace d = core::domain;
namespace w = core::wire;
using nlohmann::json;

namespace {

std::string gs(const json &j, const char *k, const std::string &def = "") {
  return core::codec::getStr(j, k, def);
}
int gi(const json &j, const char *k, int def) { return core::codec::getInt(j, k, def); }
bool gb(const json &j, const char *k, bool def) { return core::codec::getBool(j, k, def); }

const char *typeStr(d::RequestType t) {
  switch (t) {
  case d::RequestType::Http: return w::kHttp;
  case d::RequestType::Grpc: return w::kGrpc;
  case d::RequestType::GraphQl: return w::kGraphql;
  case d::RequestType::WebSocket: return w::kWs;
  case d::RequestType::Kafka: return w::kKafka;
  }
  return w::kHttp;
}

// match() dispatch helper (no shared utility header — pasted locally, same idiom as grpc_descriptors.cpp).
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// PathVariables (no core::serial codec — inline here). JSON array [{key,value,enabled}].
json pathVarsToJson(const d::PathVariableList &list) {
  json a = json::array();
  for (const auto &p : list.items())
    a.push_back({{"key", p.key()}, {"value", p.value()}, {"enabled", p.enabled() ? 1 : 0}});
  return a;
}
d::Result<d::PathVariableList> pathVarsFromJson(const json &arr) {
  std::vector<d::PathVariable> out;
  if (arr.is_array())
    for (const auto &e : arr) {
      if (!e.is_object()) continue;
      auto r = d::PathVariable::create(gs(e, "key"), gs(e, "value"), gb(e, "enabled", true));
      if (!r) return d::Result<d::PathVariableList>::fail(r.error());
      out.push_back(r.take());
    }
  return d::Result<d::PathVariableList>::ok(d::PathVariableList(std::move(out)));
}

// core::serial bridges (string API) <-> json sub-object.
template <class T, class Fn> d::Result<T> serialFrom(const json &j, const char *key, Fn fn, const char *empty) {
  auto it = j.find(key);
  return fn(it != j.end() ? it->dump() : std::string(empty));
}
json serialTo(const std::string &s) { return json::parse(s); }

// ---- TLS ----
json tlsToJson(const d::TlsConfig &t) {
  return json{{"enabled", t.enabled()},
              {"insecureSkipVerify", t.insecureSkipVerify()},
              {"caCertPath", t.caCertPath()},
              {"clientCertPath", t.clientCertPath()},
              {"clientKeyPath", t.clientKeyPath()}};
}
d::TlsConfig tlsFromJson(const json &j) {
  if (!j.is_object()) return d::TlsConfig::disabled();
  return d::TlsConfig::create(gb(j, "enabled", false), gb(j, "insecureSkipVerify", false),
                              gs(j, "caCertPath"), gs(j, "clientCertPath"), gs(j, "clientKeyPath"));
}

// ---- ProtoSource ----
json protoSourceToJson(const d::ProtoSource &ps) {
  json j;
  ps.match([&](auto &&p) {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, d::ProtoReflection>) {
      j["mode"] = "reflection";
    } else if constexpr (std::is_same_v<T, d::ProtoFiles>) {
      j["mode"] = "protoFiles";
      j["importPaths"] = p.importPaths;
      j["protoFiles"] = p.protoFiles;
    } else if constexpr (std::is_same_v<T, d::ProtoDescriptorSet>) {
      j["mode"] = "descriptorSet";
      j["path"] = p.descriptorSetPath;
    }
  });
  return j;
}
d::Result<d::ProtoSource> protoSourceFromJson(const json &j) {
  std::string mode = j.is_object() ? gs(j, "mode", "reflection") : "reflection";
  if (mode == "protoFiles") {
    std::vector<std::string> imp, files;
    if (auto it = j.find("importPaths"); it != j.end() && it->is_array())
      for (const auto &e : *it) if (e.is_string()) imp.push_back(e.get<std::string>());
    if (auto it = j.find("protoFiles"); it != j.end() && it->is_array())
      for (const auto &e : *it) if (e.is_string()) files.push_back(e.get<std::string>());
    return d::ProtoSource::files(std::move(imp), std::move(files));
  }
  if (mode == "descriptorSet") return d::ProtoSource::descriptorSet(gs(j, "path"));
  return d::Result<d::ProtoSource>::ok(d::ProtoSource::reflection());
}

// ---- GraphQL operation enum <-> string ----
const char *gqlOpStr(d::GqlOperationType o) {
  switch (o) {
  case d::GqlOperationType::Query: return "query";
  case d::GqlOperationType::Mutation: return "mutation";
  case d::GqlOperationType::Subscription: return "subscription";
  default: return "auto";
  }
}
d::GqlOperationType gqlOpFrom(const std::string &s) {
  if (s == "query") return d::GqlOperationType::Query;
  if (s == "mutation") return d::GqlOperationType::Mutation;
  if (s == "subscription") return d::GqlOperationType::Subscription;
  return d::GqlOperationType::Auto;
}

// ---- Kafka: config/message sub-blocks delegate to core::serial (SAME shapes the UI's Message/Config
// editor tabs use, field_json.cpp) — no logic duplicated between the on-disk mapper and the editor. ----
json kafkaSecurityToJson(const d::KafkaSecurity &s) {
  json j;
  s.match([&](auto &&v) {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, d::KafkaPlaintext>) j["type"] = "plaintext";
  });
  return j;
}
d::Result<d::KafkaSecurity> kafkaSecurityFromJson(const json &j) {
  std::string type = j.is_object() ? gs(j, "type", "plaintext") : "plaintext";
  if (type != "plaintext")
    return d::Result<d::KafkaSecurity>::fail(
        {d::ErrorCode::Unsupported, "unsupported kafka security type: " + type, "kafka.security.type"});
  return d::Result<d::KafkaSecurity>::ok(d::KafkaSecurity::plaintext());
}

json kafkaProducerToJson(const d::KafkaProduceSpec &p) {
  return json{{"config", serialTo(core::serial::kafkaProduceConfigToJson(p.config))},
              {"message", serialTo(core::serial::kafkaMessageToJson(p.message))}};
}
d::Result<d::KafkaProduceSpec> kafkaProducerFromJson(const json &b) {
  auto cfg = serialFrom<d::KafkaProduceConfig>(b, "config", core::serial::jsonToKafkaProduceConfig, "{}");
  if (!cfg) return d::Result<d::KafkaProduceSpec>::fail(cfg.error());
  auto msg = serialFrom<d::KafkaMessage>(b, "message", core::serial::jsonToKafkaMessage, "{}");
  if (!msg) return d::Result<d::KafkaProduceSpec>::fail(msg.error());
  return d::Result<d::KafkaProduceSpec>::ok(d::KafkaProduceSpec{cfg.take(), msg.take()});
}

json kafkaConsumerToJson(const d::KafkaConsumeSpec &spec) {
  return json{{"config", serialTo(core::serial::kafkaConsumeConfigToJson(spec.config))}};
}
d::Result<d::KafkaConsumeSpec> kafkaConsumerFromJson(const json &outer) {
  auto cfg = serialFrom<d::KafkaConsumeConfig>(outer, "config", core::serial::jsonToKafkaConsumeConfig, "{}");
  if (!cfg) return d::Result<d::KafkaConsumeSpec>::fail(cfg.error());
  return d::Result<d::KafkaConsumeSpec>::ok(d::KafkaConsumeSpec{cfg.take()});
}

json kafkaToJson(const d::KafkaRequest &k) {
  json j{{"brokers", k.brokers().toBootstrapServers()},
        {"clientKind", k.kind() == d::KafkaClientKind::Producer ? "producer" : "consumer"},
        {"security", kafkaSecurityToJson(k.security())},
        {"producer", nullptr},
        {"consumer", nullptr}};
  k.match(overloaded{
      [&](const d::KafkaProduceSpec &p) { j["producer"] = kafkaProducerToJson(p); },
      [&](const d::KafkaConsumeSpec &c) { j["consumer"] = kafkaConsumerToJson(c); },
  });
  return j;
}

// ===== per-type block builders (domain -> json) =====
json httpToJson(const d::HttpRequest &h) {
  return json{{"method", d::toString(h.method())},
              {"url", h.url().raw()},
              {"pathVariables", pathVarsToJson(h.pathVariables())},
              {"params", serialTo(core::serial::paramsToJson(h.params()))},
              {"headers", serialTo(core::serial::headersToJson(h.headers()))},
              {"body", serialTo(core::serial::bodyToJson(h.body()))},
              {"auth", serialTo(core::serial::authToJson(h.auth()))}};
}
json grpcToJson(const d::GrpcRequest &g) {
  return json{{"target", g.target()},
              {"service", g.service()},
              {"method", g.method()},
              {"methodType", d::toString(g.methodType())},
              {"message", g.message().text()},
              {"metadata", serialTo(core::serial::metadataToJson(g.metadata()))},
              {"protoSource", protoSourceToJson(g.protoSource())},
              {"tls", tlsToJson(g.tls())}};
}
json wsToJson(const d::WebSocketRequest &w) {
  // On-disk schema (json_codec compat): onOpenSend is a string[] of payloads; per-message kind == the
  // request's defaultSendKind (legacy never stored per-frame kind — matches the request bridge).
  json onOpen = json::array();
  for (const auto &m : w.onOpenSend()) onOpen.push_back(m.payload);
  return json{{"url", w.url().raw()},
              {"subprotocols", w.subprotocols()},
              {"headers", serialTo(core::serial::headersToJson(w.headers()))},
              {"auth", serialTo(core::serial::authToJson(w.auth()))},
              {"onOpenSend", onOpen},
              {"defaultSendKind", w.defaultSendKind() == d::WsSendKind::Binary ? "binary" : "text"}};
}
json gqlToJson(const d::GraphQlRequest &g) {
  // On-disk schema (json_codec compat) via the request-bridge bijection: Ws<->"ws", Http<->"sse";
  // wsProtocol "graphql-ws"<->"subscriptions-transport-ws", else "graphql-transport-ws".
  return json{{"url", g.url().raw()},
              {"query", g.op().query},
              {"operationName", g.op().operationName},
              {"variables", g.op().variables.text()},
              {"operation", gqlOpStr(g.op().operation)},
              {"headers", serialTo(core::serial::headersToJson(g.headers()))},
              {"auth", serialTo(core::serial::authToJson(g.auth()))},
              {"subTransport", g.subTransport() == d::GqlSubTransport::Ws ? "ws" : "sse"},
              {"wsProtocol", g.wsProtocol() == "graphql-ws" ? "subscriptions-transport-ws"
                                                            : "graphql-transport-ws"}};
}

// Per-type payload parsers — kept out of fromJson so it stays a flat dispatch (clang-tidy complexity).
using Payload = d::RequestModel::Payload;

d::Result<Payload> parseGrpcPayload(const json &b) {
  d::GrpcRequest::Parts p;
  p.target = gs(b, "target");
  p.service = gs(b, "service");
  p.method = gs(b, "method");
  auto mt = d::parseGrpcMethodType(gs(b, "methodType", "unary"));
  p.methodType = mt ? mt.take() : d::GrpcMethodType::Unary;
  p.message = d::JsonText::of(gs(b, "message", "{}"));
  auto md = serialFrom<d::GrpcMetadata>(b, "metadata", core::serial::jsonToMetadata, "[]");
  if (!md) return d::Result<Payload>::fail(md.error());
  p.metadata = md.take();
  auto ps = protoSourceFromJson(b.value("protoSource", json::object()));
  if (!ps) return d::Result<Payload>::fail(ps.error());
  p.protoSource = ps.take();
  p.tls = tlsFromJson(b.value("tls", json::object()));
  auto r = d::GrpcRequest::create(std::move(p));
  if (!r) return d::Result<Payload>::fail(r.error());
  return d::Result<Payload>::ok(Payload{r.take()});
}

d::Result<Payload> parseWsPayload(const json &b) {
  d::WebSocketRequest::Parts p{d::Url::create(gs(b, "url")).take()};
  p.defaultSendKind = gs(b, "defaultSendKind", "text") == "binary" ? d::WsSendKind::Binary
                                                                   : d::WsSendKind::Text;
  if (auto it = b.find("subprotocols"); it != b.end() && it->is_array())
    for (const auto &e : *it) if (e.is_string()) p.subprotocols.push_back(e.get<std::string>());
  // onOpenSend is a string[] of payloads (json_codec compat); kind = defaultSendKind.
  if (auto it = b.find("onOpenSend"); it != b.end() && it->is_array())
    for (const auto &e : *it)
      if (e.is_string()) p.onOpenSend.push_back({p.defaultSendKind, e.get<std::string>()});
  auto h = serialFrom<d::HeaderList>(b, "headers", core::serial::jsonToHeaders, "[]");
  if (!h) return d::Result<Payload>::fail(h.error());
  p.headers = h.take();
  auto a = serialFrom<d::Auth>(b, "auth", core::serial::jsonToAuth, "{\"type\":\"none\"}");
  if (!a) return d::Result<Payload>::fail(a.error());
  p.auth = a.take();
  auto r = d::WebSocketRequest::create(std::move(p));
  if (!r) return d::Result<Payload>::fail(r.error());
  return d::Result<Payload>::ok(Payload{r.take()});
}

d::Result<Payload> parseGraphqlPayload(const json &b) {
  d::GraphQlRequest::Parts p{d::Url::create(gs(b, "url")).take()};
  p.op.query = gs(b, "query");
  p.op.operationName = gs(b, "operationName");
  p.op.variables = d::JsonText::of(gs(b, "variables", "{}"));
  p.op.operation = gqlOpFrom(gs(b, "operation", "auto"));
  // json_codec compat (request-bridge bijection): "ws"->Ws, "sse"->Http; wsProtocol alias; clear for Http.
  p.subTransport = gs(b, "subTransport", "ws") == "sse" ? d::GqlSubTransport::Http : d::GqlSubTransport::Ws;
  p.wsProtocol = gs(b, "wsProtocol", "graphql-transport-ws") == "subscriptions-transport-ws"
                     ? "graphql-ws"
                     : "graphql-transport-ws";
  if (p.subTransport == d::GqlSubTransport::Http) p.wsProtocol.clear();
  auto h = serialFrom<d::HeaderList>(b, "headers", core::serial::jsonToHeaders, "[]");
  if (!h) return d::Result<Payload>::fail(h.error());
  p.headers = h.take();
  auto a = serialFrom<d::Auth>(b, "auth", core::serial::jsonToAuth, "{\"type\":\"none\"}");
  if (!a) return d::Result<Payload>::fail(a.error());
  p.auth = a.take();
  auto r = d::GraphQlRequest::create(std::move(p));
  if (!r) return d::Result<Payload>::fail(r.error());
  return d::Result<Payload>::ok(Payload{r.take()});
}

d::Result<Payload> parseKafkaPayload(const json &b) {
  auto brokers = d::BrokerList::parse(gs(b, "brokers"));
  if (!brokers) return d::Result<Payload>::fail(brokers.error());
  auto security = kafkaSecurityFromJson(b.value("security", json::object()));
  if (!security) return d::Result<Payload>::fail(security.error());

  std::string kind = gs(b, "clientKind", "producer");
  d::Result<d::KafkaRequest::Mode> mode = [&]() -> d::Result<d::KafkaRequest::Mode> {
    if (kind == "consumer") {
      auto c = kafkaConsumerFromJson(b.value("consumer", json::object()));
      if (!c) return d::Result<d::KafkaRequest::Mode>::fail(c.error());
      return d::Result<d::KafkaRequest::Mode>::ok(d::KafkaRequest::Mode{c.take()});
    }
    auto p = kafkaProducerFromJson(b.value("producer", json::object()));
    if (!p) return d::Result<d::KafkaRequest::Mode>::fail(p.error());
    return d::Result<d::KafkaRequest::Mode>::ok(d::KafkaRequest::Mode{p.take()});
  }();
  if (!mode) return d::Result<Payload>::fail(mode.error());

  auto r = d::KafkaRequest::create(brokers.take(), security.take(), mode.take());
  if (!r) return d::Result<Payload>::fail(r.error());
  return d::Result<Payload>::ok(Payload{r.take()});
}

d::Result<Payload> parseHttpPayload(const json &b) {
  auto mr = d::parseHttpMethod(gs(b, "method", "GET"));
  d::HttpRequest::Parts p{mr ? mr.take() : d::HttpMethod::Get, d::Url::create(gs(b, "url")).take()};
  auto pv = pathVarsFromJson(b.value("pathVariables", json::array()));
  if (!pv) return d::Result<Payload>::fail(pv.error());
  p.pathVariables = pv.take();
  auto pa = serialFrom<d::QueryParamList>(b, "params", core::serial::jsonToParams, "[]");
  if (!pa) return d::Result<Payload>::fail(pa.error());
  p.params = pa.take();
  auto h = serialFrom<d::HeaderList>(b, "headers", core::serial::jsonToHeaders, "[]");
  if (!h) return d::Result<Payload>::fail(h.error());
  p.headers = h.take();
  auto bd = serialFrom<d::Body>(b, "body", core::serial::jsonToBody, "{\"mode\":\"none\"}");
  if (!bd) return d::Result<Payload>::fail(bd.error());
  p.body = bd.take();
  auto a = serialFrom<d::Auth>(b, "auth", core::serial::jsonToAuth, "{\"type\":\"none\"}");
  if (!a) return d::Result<Payload>::fail(a.error());
  p.auth = a.take();
  return d::Result<Payload>::ok(Payload{d::HttpRequest::create(std::move(p)).take()});
}
} // namespace

domain::Result<domain::RequestModel> RequestJsonMapper::fromJson(const std::string &jsonText) const {
  try {
    json j = core::codec::parseGuarded(jsonText);
    d::RequestId id(gs(j, "id"));
    std::string name = gs(j, "name");
    int seq = gi(j, "seq", 0);

    d::RequestConfig cfg{d::Timeout::fromMillis(30000).take(), true};
    if (auto it = j.find("config"); it != j.end() && it->is_object()) {
      auto c = core::serial::jsonToConfig(it->dump());
      if (!c) return d::Result<d::RequestModel>::fail(c.error());
      cfg = c.take();
    }

    // Dispatch to the per-type parser (string type -> can't be a switch; block key == type token).
    std::string type = gs(j, "type", w::kHttp);
    d::Result<Payload> pr = type == w::kGrpc      ? parseGrpcPayload(j.value(w::kGrpc, json::object()))
                            : type == w::kWs      ? parseWsPayload(j.value(w::kWs, json::object()))
                            : type == w::kGraphql ? parseGraphqlPayload(j.value(w::kGraphql, json::object()))
                            : type == w::kKafka   ? parseKafkaPayload(j.value(w::kKafka, json::object()))
                                                  : parseHttpPayload(j.value(w::kHttp, json::object()));
    if (!pr) return d::Result<d::RequestModel>::fail(pr.error());

    return d::RequestModel::create(std::move(id), std::move(name), seq, cfg, pr.take());
  } catch (const std::exception &e) {
    return d::Result<d::RequestModel>::fail({d::ErrorCode::Parse, e.what(), ""});
  }
}

std::string RequestJsonMapper::toJson(const domain::RequestModel &m) const {
  json j;
  if (!m.id().get().empty()) j["id"] = m.id().get();
  j["name"] = m.name();
  j["type"] = typeStr(m.type());
  j["seq"] = m.seq();
  j["config"] = serialTo(core::serial::configToJson(m.config()));
  m.match([&](auto &&payload) {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, d::HttpRequest>) j[w::kHttp] = httpToJson(payload);
    else if constexpr (std::is_same_v<T, d::GrpcRequest>) j[w::kGrpc] = grpcToJson(payload);
    else if constexpr (std::is_same_v<T, d::WebSocketRequest>) j[w::kWs] = wsToJson(payload);
    else if constexpr (std::is_same_v<T, d::GraphQlRequest>) j[w::kGraphql] = gqlToJson(payload);
    else if constexpr (std::is_same_v<T, d::KafkaRequest>) j[w::kKafka] = kafkaToJson(payload);
  });
  return j.dump(2);
}

} // namespace core::infra
