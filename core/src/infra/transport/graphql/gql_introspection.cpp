// gql_introspection.cpp — standard introspection query, SDL printer, and the one-off HTTP runner.
#include "infra/transport/graphql/gql_introspection.hpp"

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/domain/graphql/graphql_request.hpp"
#include "core/domain/ports/driven/i_cancellation_token.hpp"
#include "core/domain/ports/driven/i_response_sink.hpp"
#include "core/domain/response/response_event.hpp"
#include "infra/transport/graphql/graphql.hpp"
#include "infra/transport/http/native_http_sender.hpp"

namespace core::gql {
namespace d = core::domain;
using nlohmann::json;

namespace {

template <class T> d::Result<T> fail(d::ErrorCode c, const std::string &msg) {
  return d::Result<T>::fail({c, msg, ""});
}

std::string gs(const json &j, const char *k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool isBuiltinScalar(const std::string &n) {
  return n == "String" || n == "Int" || n == "Float" || n == "Boolean" || n == "ID";
}
bool isBuiltinDirective(const std::string &n) {
  return n == "skip" || n == "include" || n == "deprecated" || n == "specifiedBy";
}

// TypeRef {kind,name,ofType} -> SDL type expression ("[Episode!]!").
std::string typeRef(const json &t) {
  if (!t.is_object()) return "";
  std::string kind = gs(t, "kind");
  if (kind == "NON_NULL") return typeRef(t.value("ofType", json())) + "!";
  if (kind == "LIST") return "[" + typeRef(t.value("ofType", json())) + "]";
  return gs(t, "name");
}

// """docstring""" (indented). Empty/missing description -> nothing.
void docstring(std::string &out, const json &node, const char *indent) {
  auto it = node.find("description");
  if (it == node.end() || !it->is_string()) return;
  std::string d = it->get<std::string>();
  if (d.empty()) return;
  std::string esc;
  for (std::size_t i = 0; i < d.size(); ++i) { // a """ inside a block string must be escaped
    if (d.compare(i, 3, "\"\"\"") == 0) { esc += "\\\"\"\""; i += 2; }
    else esc += d[i];
  }
  out += indent;
  out += "\"\"\"" + esc + "\"\"\"\n";
}

// Not named "quoted": std::quoted is an ADL candidate for std::string args and wins the overload.
std::string strLit(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out + "\"";
}

void deprecation(std::string &out, const json &node) {
  if (!node.value("isDeprecated", false)) return;
  std::string reason = gs(node, "deprecationReason");
  out += reason.empty() ? " @deprecated" : " @deprecated(reason: " + strLit(reason) + ")";
}

// InputValue -> "name: Type = default" (used for field args, directive args, input fields).
std::string inputValue(const json &v) {
  std::string s = gs(v, "name") + ": " + typeRef(v.value("type", json()));
  auto dv = v.find("defaultValue");
  if (dv != v.end() && dv->is_string() && !dv->get<std::string>().empty())
    s += " = " + dv->get<std::string>(); // arrives as a GraphQL literal -> verbatim
  return s;
}

std::string argList(const json &args) {
  if (!args.is_array() || args.empty()) return "";
  std::string s = "(";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) s += ", ";
    s += inputValue(args[i]);
  }
  return s + ")";
}

void printFields(std::string &out, const json &fields) {
  for (const auto &f : fields) {
    docstring(out, f, "  ");
    out += "  " + gs(f, "name") + argList(f.value("args", json::array())) + ": " +
           typeRef(f.value("type", json()));
    deprecation(out, f);
    out += "\n";
  }
}

std::string implementsClause(const json &t) {
  auto it = t.find("interfaces");
  if (it == t.end() || !it->is_array() || it->empty()) return "";
  std::string s = " implements ";
  for (std::size_t i = 0; i < it->size(); ++i) {
    if (i) s += " & ";
    s += gs((*it)[i], "name");
  }
  return s;
}

void printType(std::string &out, const json &t) {
  std::string kind = gs(t, "kind"), name = gs(t, "name");
  docstring(out, t, "");
  if (kind == "OBJECT" || kind == "INTERFACE") {
    out += (kind == "OBJECT" ? "type " : "interface ") + name + implementsClause(t) + " {\n";
    printFields(out, t.value("fields", json::array()));
    out += "}\n";
  } else if (kind == "UNION") {
    out += "union " + name + " = ";
    const json &poss = t.value("possibleTypes", json::array());
    for (std::size_t i = 0; i < poss.size(); ++i) {
      if (i) out += " | ";
      out += gs(poss[i], "name");
    }
    out += "\n";
  } else if (kind == "ENUM") {
    out += "enum " + name + " {\n";
    for (const auto &v : t.value("enumValues", json::array())) {
      docstring(out, v, "  ");
      out += "  " + gs(v, "name");
      deprecation(out, v);
      out += "\n";
    }
    out += "}\n";
  } else if (kind == "INPUT_OBJECT") {
    out += "input " + name + " {\n";
    for (const auto &f : t.value("inputFields", json::array())) {
      docstring(out, f, "  ");
      out += "  " + inputValue(f) + "\n";
    }
    out += "}\n";
  } else if (kind == "SCALAR") {
    out += "scalar " + name + "\n";
  }
}

// Extract the __schema object: {"data":{"__schema":…}} or bare {"__schema":…}. nullptr if absent.
const json *schemaOf(const json &j) {
  const json *root = &j;
  if (auto d0 = j.find("data"); d0 != j.end() && d0->is_object()) root = &*d0;
  if (auto s = root->find("__schema"); s != root->end() && s->is_object()) return &*s;
  return nullptr;
}

std::string firstGraphQlError(const json &j) {
  auto e = j.find("errors");
  if (e == j.end() || !e->is_array() || e->empty()) return "";
  std::string msg = gs((*e)[0], "message");
  return msg.empty() ? "GraphQL error" : msg;
}

} // namespace

const std::string &introspectionQuery() {
  // Classic standard query (graphql-js getIntrospectionQuery, pre-2021 field set).
  static const std::string q = R"(query IntrospectionQuery {
  __schema {
    queryType { name }
    mutationType { name }
    subscriptionType { name }
    types { ...FullType }
    directives { name description locations args { ...InputValue } }
  }
}
fragment FullType on __Type {
  kind name description
  fields(includeDeprecated: true) {
    name description
    args { ...InputValue }
    type { ...TypeRef }
    isDeprecated deprecationReason
  }
  inputFields { ...InputValue }
  interfaces { ...TypeRef }
  enumValues(includeDeprecated: true) { name description isDeprecated deprecationReason }
  possibleTypes { ...TypeRef }
}
fragment InputValue on __InputValue { name description type { ...TypeRef } defaultValue }
fragment TypeRef on __Type {
  kind name
  ofType { kind name ofType { kind name ofType { kind name ofType {
    kind name ofType { kind name ofType { kind name ofType { kind name } } } } } } }
})";
  return q;
}

d::Result<std::string> sdlFromIntrospectionJson(const std::string &body) {
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception &e) {
    return fail<std::string>(d::ErrorCode::Parse, std::string("response is not JSON: ") + e.what());
  }
  if (!j.is_object()) return fail<std::string>(d::ErrorCode::Parse, "response is not a JSON object");
  if (std::string msg = firstGraphQlError(j); !msg.empty())
    return fail<std::string>(d::ErrorCode::Network, msg); // e.g. introspection disabled server-side
  const json *schema = schemaOf(j);
  if (!schema) return fail<std::string>(d::ErrorCode::Parse, "no __schema in response");

  std::string out;

  // schema { … } block only when a root type name deviates from the convention.
  std::string q = gs(schema->value("queryType", json()), "name");
  std::string m = gs(schema->value("mutationType", json()), "name");
  std::string s = gs(schema->value("subscriptionType", json()), "name");
  bool defaultRoots = (q.empty() || q == "Query") && (m.empty() || m == "Mutation") &&
                      (s.empty() || s == "Subscription");
  if (!defaultRoots) {
    out += "schema {\n";
    if (!q.empty()) out += "  query: " + q + "\n";
    if (!m.empty()) out += "  mutation: " + m + "\n";
    if (!s.empty()) out += "  subscription: " + s + "\n";
    out += "}\n\n";
  }

  for (const auto &dir : schema->value("directives", json::array())) {
    std::string name = gs(dir, "name");
    if (name.empty() || isBuiltinDirective(name)) continue;
    docstring(out, dir, "");
    out += "directive @" + name + argList(dir.value("args", json::array())) + " on ";
    const json &locs = dir.value("locations", json::array());
    for (std::size_t i = 0; i < locs.size(); ++i) {
      if (i) out += " | ";
      if (locs[i].is_string()) out += locs[i].get<std::string>();
    }
    out += "\n\n";
  }

  bool first = true;
  for (const auto &t : schema->value("types", json::array())) { // server order preserved
    std::string name = gs(t, "name");
    if (name.empty() || name.rfind("__", 0) == 0) continue; // introspection meta types
    if (gs(t, "kind") == "SCALAR" && isBuiltinScalar(name)) continue;
    if (!first) out += "\n";
    first = false;
    printType(out, t);
  }
  return d::Result<std::string>::ok(std::move(out));
}

d::Result<d::GqlSchema> runIntrospection(const d::RequestModel &resolved) {
  if (resolved.type() != d::RequestType::GraphQl)
    return fail<d::GqlSchema>(d::ErrorCode::Validation, "not a graphql request");
  const auto &g = std::get<d::GraphQlRequest>(resolved.payload());

  std::string url = g.url().raw();
  if (url.empty())
    return fail<d::GqlSchema>(d::ErrorCode::Validation, "graphql endpoint url required");
  // Introspection is always an HTTP POST — a subscription request's ws(s):// endpoint maps to http(s).
  if (url.rfind("ws://", 0) == 0) url = "http://" + url.substr(5);
  else if (url.rfind("wss://", 0) == 0) url = "https://" + url.substr(6);

  d::GraphQlOperation op;
  op.query = introspectionQuery();
  op.operationName = "IntrospectionQuery";
  op.operation = d::GqlOperationType::Query;
  d::GraphQlRequest::Parts gp{d::Url::create(url).take(), std::move(op), g.headers(), g.auth(),
                              d::GqlSubTransport::Http, ""};
  auto gr = d::GraphQlRequest::create(std::move(gp));
  if (!gr.isOk()) return fail<d::GqlSchema>(gr.error().code, gr.error().message);
  auto model = d::RequestModel::create(resolved.id(), resolved.name(), resolved.seq(), resolved.config(),
                                       gr.take());
  if (!model.isOk()) return fail<d::GqlSchema>(model.error().code, model.error().message);

  // Local sender + sink + never-cancel token: the shared senders hold the ACTIVE request's cancel token,
  // so this out-of-band call must not touch them (cross-cancel hazard).
  struct CaptureSink final : d::IResponseSink {
    std::optional<d::ApiResponse> ok;
    std::optional<d::ApiError> err;
    void emit(const d::ResponseEvent &ev) override {
      if (const auto *c = ev.get<d::EvCompleted>()) ok = c->summary;
      else if (const auto *f = ev.get<d::EvFailed>()) err = f->error;
    }
  } sink;
  struct NeverCancel final : d::ICancellationToken {
    bool cancelled() const noexcept override { return false; }
  } cancel;

  infra::NativeHttpSender http;
  d::Status st = http.execute(buildHttpModel(model.take()), sink, cancel);

  if (sink.err) return fail<d::GqlSchema>(d::ErrorCode::Network, sink.err->message);
  if (!sink.ok)
    return fail<d::GqlSchema>(d::ErrorCode::Network,
                              st ? std::string("no response") : st.error().message);
  const d::ApiResponse &resp = *sink.ok;
  if (resp.statusCode >= 400)
    return fail<d::GqlSchema>(d::ErrorCode::Network,
                              "HTTP " + std::to_string(resp.statusCode) + ": " + resp.body.substr(0, 200));

  auto sdl = sdlFromIntrospectionJson(resp.body);
  if (!sdl.isOk()) return d::Result<d::GqlSchema>::fail(sdl.error());

  // Raw view: pretty-print the data object (fall back to the raw body if the shape surprises us —
  // sdlFromIntrospectionJson already vouched for parseability).
  std::string pretty = resp.body;
  try {
    json j = json::parse(resp.body);
    pretty = (j.contains("data") && j["data"].is_object() ? j["data"] : j).dump(2);
  } catch (...) {}

  d::GqlSchema out;
  out.sdl = sdl.take();
  out.json = std::move(pretty);
  return d::Result<d::GqlSchema>::ok(std::move(out));
}

} // namespace core::gql
