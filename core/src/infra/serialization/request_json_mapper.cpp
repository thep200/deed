#include "infra/serialization/request_json_mapper.hpp"

#include <nlohmann/json.hpp>

#include "infra/serialization/json_codec.hpp"               // core::codec::parseGuarded — JSON nesting-depth guard (H5)
#include "core/infra/serialization/field_json.hpp"  // core::serial — domain field codecs (headers/auth/body/...)

namespace core::infra {
namespace d = core::domain;
using nlohmann::json;

namespace {

std::string gs(const json &j, const char *k, const std::string &def = "") {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
int gi(const json &j, const char *k, int def) {
  auto it = j.find(k);
  return (it != j.end() && it->is_number_integer()) ? it->get<int>() : def;
}
bool gb(const json &j, const char *k, bool def) {
  auto it = j.find(k);
  if (it == j.end()) return def;
  if (it->is_boolean()) return it->get<bool>();
  if (it->is_number()) return it->get<double>() != 0;
  return def;
}

const char *typeStr(d::RequestType t) {
  switch (t) {
  case d::RequestType::Http: return "http";
  case d::RequestType::Grpc: return "grpc";
  case d::RequestType::GraphQl: return "graphql";
  case d::RequestType::WebSocket: return "ws";
  }
  return "http";
}

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

    std::string type = gs(j, "type", "http");
    // placeholder payload (overwritten per type below; Parts holds a non-default Url -> brace-init).
    d::RequestModel::Payload payload =
        d::HttpRequest::create(d::HttpRequest::Parts{d::HttpMethod::Get, d::Url::create("").take()}).take();

    if (type == "grpc") {
      const json b = j.value("grpc", json::object());
      d::GrpcRequest::Parts p;
      p.target = gs(b, "target");
      p.service = gs(b, "service");
      p.method = gs(b, "method");
      auto mt = d::parseGrpcMethodType(gs(b, "methodType", "unary"));
      p.methodType = mt ? mt.take() : d::GrpcMethodType::Unary;
      p.message = d::JsonText::of(gs(b, "message", "{}"));
      auto md = serialFrom<d::GrpcMetadata>(b, "metadata", core::serial::jsonToMetadata, "[]");
      if (!md) return d::Result<d::RequestModel>::fail(md.error());
      p.metadata = md.take();
      auto ps = protoSourceFromJson(b.value("protoSource", json::object()));
      if (!ps) return d::Result<d::RequestModel>::fail(ps.error());
      p.protoSource = ps.take();
      p.tls = tlsFromJson(b.value("tls", json::object()));
      auto r = d::GrpcRequest::create(std::move(p));
      if (!r) return d::Result<d::RequestModel>::fail(r.error());
      payload = r.take();
    } else if (type == "ws") {
      const json b = j.value("ws", json::object());
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
      if (!h) return d::Result<d::RequestModel>::fail(h.error());
      p.headers = h.take();
      auto a = serialFrom<d::Auth>(b, "auth", core::serial::jsonToAuth, "{\"type\":\"none\"}");
      if (!a) return d::Result<d::RequestModel>::fail(a.error());
      p.auth = a.take();
      auto r = d::WebSocketRequest::create(std::move(p));
      if (!r) return d::Result<d::RequestModel>::fail(r.error());
      payload = r.take();
    } else if (type == "graphql") {
      const json b = j.value("graphql", json::object());
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
      if (!h) return d::Result<d::RequestModel>::fail(h.error());
      p.headers = h.take();
      auto a = serialFrom<d::Auth>(b, "auth", core::serial::jsonToAuth, "{\"type\":\"none\"}");
      if (!a) return d::Result<d::RequestModel>::fail(a.error());
      p.auth = a.take();
      auto r = d::GraphQlRequest::create(std::move(p));
      if (!r) return d::Result<d::RequestModel>::fail(r.error());
      payload = r.take();
    } else { // http
      const json b = j.value("http", json::object());
      auto mr = d::parseHttpMethod(gs(b, "method", "GET"));
      d::HttpRequest::Parts p{mr ? mr.take() : d::HttpMethod::Get, d::Url::create(gs(b, "url")).take()};
      auto pv = pathVarsFromJson(b.value("pathVariables", json::array()));
      if (!pv) return d::Result<d::RequestModel>::fail(pv.error());
      p.pathVariables = pv.take();
      auto pa = serialFrom<d::QueryParamList>(b, "params", core::serial::jsonToParams, "[]");
      if (!pa) return d::Result<d::RequestModel>::fail(pa.error());
      p.params = pa.take();
      auto h = serialFrom<d::HeaderList>(b, "headers", core::serial::jsonToHeaders, "[]");
      if (!h) return d::Result<d::RequestModel>::fail(h.error());
      p.headers = h.take();
      auto bd = serialFrom<d::Body>(b, "body", core::serial::jsonToBody, "{\"mode\":\"none\"}");
      if (!bd) return d::Result<d::RequestModel>::fail(bd.error());
      p.body = bd.take();
      auto a = serialFrom<d::Auth>(b, "auth", core::serial::jsonToAuth, "{\"type\":\"none\"}");
      if (!a) return d::Result<d::RequestModel>::fail(a.error());
      p.auth = a.take();
      payload = d::HttpRequest::create(std::move(p)).take();
    }

    return d::RequestModel::create(std::move(id), std::move(name), seq, cfg, std::move(payload));
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
    if constexpr (std::is_same_v<T, d::HttpRequest>) j["http"] = httpToJson(payload);
    else if constexpr (std::is_same_v<T, d::GrpcRequest>) j["grpc"] = grpcToJson(payload);
    else if constexpr (std::is_same_v<T, d::WebSocketRequest>) j["ws"] = wsToJson(payload);
    else if constexpr (std::is_same_v<T, d::GraphQlRequest>) j["graphql"] = gqlToJson(payload);
  });
  return j.dump(2);
}

} // namespace core::infra
