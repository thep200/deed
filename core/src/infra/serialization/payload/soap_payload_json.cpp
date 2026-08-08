#include "infra/serialization/payload/payload_json.hpp"

#include "core/infra/serialization/field_json.hpp"

namespace core::infra::payload {

json toJson(const d::SoapRequest &s) {
  return json{{"url", s.url().raw()},
              {"action", s.action()},
              {"version", s.version() == d::SoapVersion::V1_2 ? "1.2" : "1.1"},
              {"envelope", s.envelope()},
              {"headers", serialTo(core::serial::headersToJson(s.headers()))},
              {"auth", serialTo(core::serial::authToJson(s.auth()))}};
}

d::Result<Payload> parse(d::TypeTag<d::SoapRequest>, const json &b) {
  d::SoapRequest::Parts p{d::Url::create(gs(b, "url")).take()};
  p.action = gs(b, "action");
  p.version = gs(b, "version", "1.1") == "1.2" ? d::SoapVersion::V1_2 : d::SoapVersion::V1_1;
  p.envelope = gs(b, "envelope");
  auto h = serialFrom<d::HeaderList>(b, "headers", core::serial::jsonToHeaders, "[]");
  if (!h) return d::Result<Payload>::fail(h.error());
  p.headers = h.take();
  auto a = serialFrom<d::Auth>(b, "auth", core::serial::jsonToAuth, "{\"type\":\"none\"}");
  if (!a) return d::Result<Payload>::fail(a.error());
  p.auth = a.take();
  auto r = d::SoapRequest::create(std::move(p));
  if (!r) return d::Result<Payload>::fail(r.error());
  return d::Result<Payload>::ok(Payload{r.take()});
}

} // namespace core::infra::payload
