#include "infra/serialization/request_json_mapper.hpp"

#include <nlohmann/json.hpp>

#include "core/domain/request/request_traits.hpp"
#include "core/infra/serialization/field_json.hpp"
#include "infra/serialization/json_codec.hpp"
#include "infra/serialization/payload/payload_json.hpp"

namespace core::infra {
namespace d = core::domain;
using nlohmann::json;

domain::Result<domain::RequestModel> RequestJsonMapper::fromJson(const std::string &jsonText) const {
  try {
    json j = core::codec::parseGuarded(jsonText);
    d::RequestId id(core::codec::getStr(j, "id", ""));
    std::string name = core::codec::getStr(j, "name", "");
    int seq = core::codec::getInt(j, "seq", 0);

    d::RequestConfig cfg{d::Timeout::fromMillis(30000).take(), true};
    if (auto it = j.find("config"); it != j.end() && it->is_object()) {
      auto c = core::serial::jsonToConfig(it->dump());
      if (!c) return d::Result<d::RequestModel>::fail(c.error());
      cfg = c.take();
    }

    // "type" absent/empty -> http (legacy files); present-but-unknown -> hard error, never a silent empty HTTP request.
    std::string token = core::codec::getStr(j, "type", "");
    d::RequestType type = d::RequestType::Http;
    if (!token.empty() && !core::parseRequestType(token, type))
      return d::Result<d::RequestModel>::fail(
          {d::ErrorCode::Parse, "unknown request type: " + token, "type"});

    // block key == type token; per-type codecs resolve by overload
    auto pr = d::dispatchType(type, [&](auto tag) {
      return payload::parse(tag, j.value(core::toString(type), json::object()));
    });
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
  j["type"] = core::toString(m.type());
  j["seq"] = m.seq();
  j["config"] = json::parse(core::serial::configToJson(m.config()));
  m.match([&](const auto &p) { j[core::toString(m.type())] = payload::toJson(p); });
  return j.dump(2);
}

} // namespace core::infra
