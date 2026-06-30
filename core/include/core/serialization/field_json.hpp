// core/serialization/field_json.hpp — per-field JSON <-> DOMAIN value objects, for the editor's tabs
// (REFACTOR_SPEC P6, Phase E enabler). The domain replacement for the legacy core::fieldcodec: the UI shows
// each tab (Params/Headers/Auth/Body/Config) as a JSON box; these helpers parse that box into domain VOs
// (validated via Result) and format VOs back. nlohmann lives ONLY in the .cpp — this PUBLIC header (outside
// domain/ and app/) stays dependency-clean so the UI can include it. JSON shapes match the legacy fieldcodec
// (array of {key,value,enabled}; auth {type,...}; body {mode,...}; config {timeout_ms,tls}).
#pragma once

#include <string>

#include <vector>

#include "core/domain/auth/auth.hpp"
#include "core/domain/body/body.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/grpc/grpc_metadata.hpp"
#include "core/domain/request/request_config.hpp"
#include "core/domain/response/api_response.hpp"
#include "core/domain/values/header.hpp"
#include "core/domain/values/query_param.hpp"

namespace core::serial {

// Headers — JSON array [{key,value,enabled}] <-> HeaderList.
std::string headersToJson(const domain::HeaderList &);
domain::Result<domain::HeaderList> jsonToHeaders(const std::string &);

// Query params — JSON array [{key,value,enabled}] <-> QueryParamList.
std::string paramsToJson(const domain::QueryParamList &);
domain::Result<domain::QueryParamList> jsonToParams(const std::string &);

// Auth — JSON {type, basic{username,password} | bearer{token} | apikey{key,value,in}} <-> Auth.
std::string authToJson(const domain::Auth &);
domain::Result<domain::Auth> jsonToAuth(const std::string &);

// Body — JSON {mode, json|text|xml | formUrlEncoded[] | multipart[] | binary{filePath}} <-> Body.
std::string bodyToJson(const domain::Body &);
domain::Result<domain::Body> jsonToBody(const std::string &);

// Editor body view: the UI body tab shows ONE mode's UNWRAPPED content (the {mode} lives in the app, not the
// text). bodyToEditor decomposes a domain Body into (mode, content); bodyFromEditor rebuilds it. Modes:
// json|text|xml (raw text), form-urlencoded (array [{key,value,enabled}]), binary ({"filePath":...}).
struct EditorBody {
  std::string mode;    // "json" | "text" | "xml" | "form-urlencoded" | "binary"
  std::string content; // unwrapped text for that mode
};
EditorBody bodyToEditor(const domain::Body &);
domain::Result<domain::Body> bodyFromEditor(const std::string &mode, const std::string &content);

// Config — JSON {timeout_ms, tls} <-> RequestConfig.
std::string configToJson(const domain::RequestConfig &);
domain::Result<domain::RequestConfig> jsonToConfig(const std::string &);

// gRPC metadata — JSON array [{key,value,enabled}] <-> GrpcMetadata.
std::string metadataToJson(const domain::GrpcMetadata &);
domain::Result<domain::GrpcMetadata> jsonToMetadata(const std::string &);

// Response headers — domain ResponseHeader list -> JSON array [{key,value}] (display only; read-only).
std::string responseHeadersToJson(const std::vector<domain::ResponseHeader> &);

// Generic JSON text helpers (no domain types): pretty/compact reformat; encode/decode a JSON string literal.
std::string formatJson(const std::string &text, bool pretty); // unparseable -> returned verbatim
std::string jsonEncodeString(const std::string &text);        // text -> "\"...\""
std::string jsonDecodeString(const std::string &text);        // "\"...\"" -> inner; else verbatim

} // namespace core::serial
