#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/domain/request/request_model.hpp"
#include "core/domain/request/request_traits.hpp"
#include "infra/serialization/json_codec.hpp"

namespace core::infra::payload {

namespace d = core::domain;
using nlohmann::json;
using Payload = d::RequestModel::Payload;

inline std::string gs(const json &j, const char *k, const std::string &def = "") {
  return core::codec::getStr(j, k, def);
}
inline int gi(const json &j, const char *k, int def) { return core::codec::getInt(j, k, def); }
inline bool gb(const json &j, const char *k, bool def) { return core::codec::getBool(j, k, def); }

// core::serial bridges (string API) <-> json sub-object
template <class T, class Fn>
d::Result<T> serialFrom(const json &j, const char *key, Fn fn, const char *empty) {
  auto it = j.find(key);
  return fn(it != j.end() ? it->dump() : std::string(empty));
}
inline json serialTo(const std::string &s) { return json::parse(s); }

// One pair per Payload alternative, resolved by overload — a new type without its pair breaks the mapper at compile time.
json toJson(const d::HttpRequest &);
json toJson(const d::GrpcRequest &);
json toJson(const d::GraphQlRequest &);
json toJson(const d::WebSocketRequest &);
json toJson(const d::KafkaRequest &);
json toJson(const d::SoapRequest &);
json toJson(const d::LdapRequest &);

d::Result<Payload> parse(d::TypeTag<d::HttpRequest>, const json &block);
d::Result<Payload> parse(d::TypeTag<d::GrpcRequest>, const json &block);
d::Result<Payload> parse(d::TypeTag<d::GraphQlRequest>, const json &block);
d::Result<Payload> parse(d::TypeTag<d::WebSocketRequest>, const json &block);
d::Result<Payload> parse(d::TypeTag<d::KafkaRequest>, const json &block);
d::Result<Payload> parse(d::TypeTag<d::SoapRequest>, const json &block);
d::Result<Payload> parse(d::TypeTag<d::LdapRequest>, const json &block);

} // namespace core::infra::payload
