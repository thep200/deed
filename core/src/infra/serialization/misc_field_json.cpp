#include "core/infra/serialization/field_json.hpp"

#include <nlohmann/json.hpp>

#include "infra/serialization/field_json_common.hpp"

namespace core::serial {
namespace d = core::domain;

std::string metadataToJson(const d::GrpcMetadata &md) {
  json a = json::array();
  for (const auto &e : md.entries())
    a.push_back({{"key", e.key}, {"value", e.value}, {"enabled", e.enabled ? 1 : 0}});
  return a.dump(2);
}
d::Result<d::GrpcMetadata> jsonToMetadata(const std::string &text) {
  try {
    auto j = parseGuarded(text, "[]");
    if (!j.is_array()) return parseErr<d::GrpcMetadata>("metadata must be a JSON array");
    std::vector<d::MetadataEntry> entries;
    for (const auto &e : j) {
      if (!e.is_object()) continue;
      entries.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
    }
    return d::GrpcMetadata::create(std::move(entries));
  } catch (const std::exception &e) {
    return parseErr<d::GrpcMetadata>(e.what());
  }
}

std::string soapConfigToJson(const d::SoapRequest &s) {
  json j{{"action", s.action()},
        {"version", s.version() == d::SoapVersion::V1_2 ? "1.2" : "1.1"}};
  return j.dump(2);
}
d::Result<SoapConfig> jsonToSoapConfig(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    SoapConfig c;
    c.action = gs(j, "action");
    std::string v = gs(j, "version", "1.1");
    if (v == "1.2") c.version = d::SoapVersion::V1_2;
    else if (v != "1.1") return parseErr<SoapConfig>("unknown soap version: " + v + " (want 1.1 or 1.2)");
    return d::Result<SoapConfig>::ok(std::move(c));
  } catch (const std::exception &e) {
    return parseErr<SoapConfig>(e.what());
  }
}

std::string ldapParamsToJson(const d::LdapRequest &l) {
  json j{{"startTls", l.startTls()},
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
  return j.dump(2);
}
d::Result<LdapParams> jsonToLdapParams(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    LdapParams p;
    p.startTls = gb(j, "startTls", false);
    p.bindDn = gs(j, "bindDn");
    p.bindPassword = gs(j, "bindPassword");
    p.baseDn = gs(j, "baseDn");
    std::string sc = gs(j, "scope", "sub");
    if (!d::parseLdapScope(sc, p.scope))
      return parseErr<LdapParams>("unknown scope: " + sc + " (want base|one|sub)");
    p.filter = gs(j, "filter");
    if (auto it = j.find("attributes"); it != j.end() && it->is_array())
      for (const auto &e : *it) if (e.is_string()) p.attributes.push_back(e.get<std::string>());
    p.group = gs(j, "group");
    p.testPassword = gs(j, "testPassword");
    p.sizeLimit = gi(j, "sizeLimit", 100);
    p.timeLimit = gi(j, "timeLimit", 10);
    p.pageSize = gi(j, "pageSize", 500);
    if (p.sizeLimit < 0 || p.timeLimit < 0 || p.pageSize < 0)
      return parseErr<LdapParams>("limits must be >= 0");
    return d::Result<LdapParams>::ok(std::move(p));
  } catch (const std::exception &e) {
    return parseErr<LdapParams>(e.what());
  }
}

} // namespace core::serial
