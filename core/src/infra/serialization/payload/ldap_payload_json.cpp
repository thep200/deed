#include "infra/serialization/payload/payload_json.hpp"

namespace core::infra::payload {

json toJson(const d::LdapRequest &l) {
  return json{{"url", l.url().raw()},
              {"startTls", l.startTls()},
              {"bindDn", l.bindDn()},
              {"bindPassword", l.bindPassword()},
              {"baseDn", l.baseDn()},
              {"scope", d::toString(l.scope())},
              {"filter", l.filter()},
              {"attributes", l.attributes()},
              {"group", l.group()},
              {"testPassword", l.testPassword()},
              {"sizeLimit", l.sizeLimit()},
              {"timeLimit", l.timeLimit()},
              {"pageSize", l.pageSize()}};
}

d::Result<Payload> parse(d::TypeTag<d::LdapRequest>, const json &b) {
  d::LdapRequest::Parts p{d::Url::create(gs(b, "url")).take()};
  p.startTls = gb(b, "startTls", false);
  p.bindDn = gs(b, "bindDn");
  p.bindPassword = gs(b, "bindPassword");
  p.baseDn = gs(b, "baseDn");
  if (!d::parseLdapScope(gs(b, "scope", "sub"), p.scope)) p.scope = d::LdapScope::Sub;
  p.filter = gs(b, "filter");
  if (auto it = b.find("attributes"); it != b.end() && it->is_array())
    for (const auto &e : *it)
      if (e.is_string()) p.attributes.push_back(e.get<std::string>());
  p.group = gs(b, "group");
  p.testPassword = gs(b, "testPassword");
  p.sizeLimit = gi(b, "sizeLimit", 100);
  p.timeLimit = gi(b, "timeLimit", 10);
  p.pageSize = gi(b, "pageSize", 500);
  auto r = d::LdapRequest::create(std::move(p));
  if (!r) return d::Result<Payload>::fail(r.error());
  return d::Result<Payload>::ok(Payload{r.take()});
}

} // namespace core::infra::payload
