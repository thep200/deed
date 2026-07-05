// json_codec.hpp — convert Environment/AppConfig/Session <-> JSON (README §7) + parseGuarded.
// Core-internal; nlohmann::json does not leak into the public port. (RequestModel JSON now lives in
// the domain mapper `request_json_mapper`; this codec is config-only.)
#pragma once

#include <nlohmann/json.hpp>

#include "core/domain/environment/env_config.hpp" // Environment / AppConfig / Session config PODs

namespace core::codec {

using json = nlohmann::json;

// Maximum JSON nesting depth accepted by parseGuarded (H5).
inline constexpr int kMaxJsonDepth = 200;

// Parse JSON with a nesting-depth guard (H5). nlohmann's recursive descent parser can overflow the stack
// on pathologically deep input (e.g. thousands of "[[[…") — a crash that a try/catch around json::parse
// CANNOT recover. This pre-scans structural depth (cheap, O(n), ignores brackets inside strings) and throws
// a catchable nlohmann::json::parse_error before handing off to json::parse. Use for ALL untrusted/on-disk
// text (imported/pasted requests, cached files, env/collection files).
json parseGuarded(const std::string& text, int maxDepth = kMaxJsonDepth);

// ---- safe helpers (missing key -> default) ----
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

// Environment (non-secret part only; secrets flagged, value empty)
json toJson(const Environment&);
Environment envFromJson(const json&);

// AppConfig / Session
json toJson(const AppConfig&);
AppConfig appConfigFromJson(const json&);
// Same as above but missing keys fall back to `defaults` (values from .env) instead of hard-coded constants.
AppConfig appConfigFromJson(const json&, const AppConfig& defaults);
json toJson(const Session&);
Session sessionFromJson(const json&);

} // namespace core::codec
