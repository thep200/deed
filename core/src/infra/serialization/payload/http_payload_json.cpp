#include "infra/serialization/payload/payload_json.hpp"

#include "core/infra/serialization/field_json.hpp"

namespace core::infra::payload {

namespace {

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

} // namespace

json toJson(const d::HttpRequest &h) {
  return json{{"method", d::toString(h.method())},
              {"url", h.url().raw()},
              {"pathVariables", pathVarsToJson(h.pathVariables())},
              {"params", serialTo(core::serial::paramsToJson(h.params()))},
              {"headers", serialTo(core::serial::headersToJson(h.headers()))},
              {"body", serialTo(core::serial::bodyToJson(h.body()))},
              {"auth", serialTo(core::serial::authToJson(h.auth()))}};
}

d::Result<Payload> parse(d::TypeTag<d::HttpRequest>, const json &b) {
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

} // namespace core::infra::payload
