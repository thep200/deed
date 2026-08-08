#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/domain/common/result.hpp"
#include "infra/serialization/json_codec.hpp"

namespace core::serial {

using nlohmann::json;

inline std::string gs(const json &j, const char *k, const std::string &dflt = "") {
  return core::codec::getStr(j, k, dflt);
}
inline bool gb(const json &j, const char *k, bool dflt) { return core::codec::getBool(j, k, dflt); }
inline int gi(const json &j, const char *k, int dflt) { return core::codec::getInt(j, k, dflt); }
inline json parseGuarded(const std::string &text, const char *fallback) {
  return core::codec::parseGuarded(text.empty() ? fallback : text); // depth-guarded
}
template <class T> core::domain::Result<T> parseErr(const std::string &what) {
  return core::domain::Result<T>::fail({core::domain::ErrorCode::Parse, what, ""});
}

} // namespace core::serial
