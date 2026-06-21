// json_codec.hpp — convert RequestModel/Environment <-> JSON (README §7).
// Core-internal; nlohmann::json does not leak into the public port.
#pragma once

#include <nlohmann/json.hpp>

#include "core/cache.hpp"
#include "core/types.hpp"

namespace core::codec {

using json = nlohmann::json;

// RequestModel
json toJson(const RequestModel&);
RequestModel requestFromJson(const json&);
std::string dumpRequest(const RequestModel&); // pretty (2 space)

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

// ResponseRecord (L2 disk cache) — includes ApiResponse + error + meta.
json toJson(const ResponseRecord&);
ResponseRecord responseRecordFromJson(const json&);

} // namespace core::codec
