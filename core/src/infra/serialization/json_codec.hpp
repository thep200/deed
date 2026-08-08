#pragma once

#include <nlohmann/json.hpp>

#include "core/domain/environment/env_config.hpp"

namespace core::codec {

using json = nlohmann::json;

inline constexpr int kMaxJsonDepth = 200;

// nlohmann's recursive parser can overflow the stack on deep input ("[[[…") — a crash no try/catch
// recovers — so pre-scan depth and throw a catchable error first. Use for ALL untrusted/on-disk text.
json parseGuarded(const std::string& text, int maxDepth = kMaxJsonDepth);

// missing key -> default
inline std::string getStr(const json& j, const char* k, const std::string& def = "") {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
inline int getInt(const json& j, const char* k, int def = 0) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number_integer()) ? it->get<int>() : def;
}
inline bool getBool(const json& j, const char* k, bool def = false) {
    auto it = j.find(k);
    if (it == j.end()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;   // accept 0/1
    return def;
}

json toJson(const Environment&);
Environment envFromJson(const json&);

json toJson(const AppConfig&);
AppConfig appConfigFromJson(const json&);
// Missing keys fall back to `defaults` (from .env) instead of hard-coded constants.
AppConfig appConfigFromJson(const json&, const AppConfig& defaults);
json toJson(const Session&);
Session sessionFromJson(const json&);

} // namespace core::codec
