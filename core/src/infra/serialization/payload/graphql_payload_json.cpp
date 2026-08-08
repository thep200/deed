#include "infra/serialization/payload/payload_json.hpp"

#include "core/infra/serialization/field_json.hpp"

namespace core::infra::payload {

namespace {

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

} // namespace

json toJson(const d::GraphQlRequest &g) {
  // json_codec-compat wire shape: Ws<->"ws", Http<->"sse"; wsProtocol "graphql-ws"<->
  // "subscriptions-transport-ws", else "graphql-transport-ws".
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

d::Result<Payload> parse(d::TypeTag<d::GraphQlRequest>, const json &b) {
  d::GraphQlRequest::Parts p{d::Url::create(gs(b, "url")).take()};
  p.op.query = gs(b, "query");
  p.op.operationName = gs(b, "operationName");
  p.op.variables = d::JsonText::of(gs(b, "variables", "{}"));
  p.op.operation = gqlOpFrom(gs(b, "operation", "auto"));
  // json_codec compat: "ws"->Ws, "sse"->Http; wsProtocol alias; cleared for Http.
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

} // namespace core::infra::payload
