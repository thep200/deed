// json_codec.hpp — chuyển đổi RequestModel/Environment <-> JSON (README §7).
// Nội bộ Core; nlohmann::json không leak ra port public.
#pragma once

#include <nlohmann/json.hpp>

#include "core/types.hpp"

namespace core::codec {

using json = nlohmann::json;

// RequestModel
json toJson(const RequestModel&);
RequestModel requestFromJson(const json&);
std::string dumpRequest(const RequestModel&); // pretty (2 space)

// Environment (chỉ phần non-secret; secret đánh dấu cờ, value rỗng)
json toJson(const Environment&);
Environment envFromJson(const json&);

// AppConfig / Session
json toJson(const AppConfig&);
AppConfig appConfigFromJson(const json&);
json toJson(const Session&);
Session sessionFromJson(const json&);

} // namespace core::codec
