// core/field_codec.hpp — round-trip raw JSON per field for the editor (UI spec
// §3). UI shows each tab (Params/Headers/Body/Auth/Metadata/Message) as a JSON
// box mapping straight to a field. This is a helper port so UI need not parse JSON itself.
#pragma once

#include <string>
#include <vector>

#include "core/types.hpp"

namespace core::fieldcodec {

// KeyValue[] (headers/params/metadata) <-> JSON text.
std::string keyValuesToJson(const std::vector<KeyValue> &);
bool jsonToKeyValues(const std::string &, std::vector<KeyValue> &out,
                     std::string &err);

// Body{} <-> JSON text.
std::string bodyToJson(const Body &);
bool jsonToBody(const std::string &, Body &out, std::string &err);

// Auth{} <-> JSON text.
std::string authToJson(const Auth &);
bool jsonToAuth(const std::string &, Auth &out, std::string &err);

// RequestConfig{} <-> JSON text (Config tab: timeout_ms + tls).
std::string configToJson(const RequestConfig &);
bool jsonToConfig(const std::string &, RequestConfig &out, std::string &err);

// Format JSON: pretty (indent 2) or compact. Unparseable -> return verbatim.
std::string formatJson(const std::string &text, bool pretty);

// Encode: wrap text into an escaped JSON string literal ("\"...\"").
std::string jsonEncodeString(const std::string &text);
// Decode: if text is a JSON string literal -> return inner content; otherwise
// keep as-is.
std::string jsonDecodeString(const std::string &text);

} // namespace core::fieldcodec
