#include "infra/serialization/payload/payload_json.hpp"

#include "core/infra/serialization/field_json.hpp"

namespace core::infra::payload {

json toJson(const d::WebSocketRequest &w) {
  // json_codec compat: onOpenSend is a string[] of payloads; per-message kind == defaultSendKind
  // (legacy files never stored a per-frame kind).
  json onOpen = json::array();
  for (const auto &m : w.onOpenSend()) onOpen.push_back(m.payload);
  return json{{"url", w.url().raw()},
              {"subprotocols", w.subprotocols()},
              {"headers", serialTo(core::serial::headersToJson(w.headers()))},
              {"auth", serialTo(core::serial::authToJson(w.auth()))},
              {"onOpenSend", onOpen},
              {"defaultSendKind", w.defaultSendKind() == d::WsSendKind::Binary ? "binary" : "text"}};
}

d::Result<Payload> parse(d::TypeTag<d::WebSocketRequest>, const json &b) {
  d::WebSocketRequest::Parts p{d::Url::create(gs(b, "url")).take()};
  p.defaultSendKind = gs(b, "defaultSendKind", "text") == "binary" ? d::WsSendKind::Binary
                                                                   : d::WsSendKind::Text;
  if (auto it = b.find("subprotocols"); it != b.end() && it->is_array())
    for (const auto &e : *it)
      if (e.is_string()) p.subprotocols.push_back(e.get<std::string>());
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

} // namespace core::infra::payload
