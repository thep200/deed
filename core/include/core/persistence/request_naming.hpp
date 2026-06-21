// request_naming.hpp — request file naming convention (new_format.file.md §2A; updated LAZY_TREE §2).
// The filename is a DERIVED CACHE of the content: lets us build the tree + get id WITHOUT reading the file.
//   HTTP:  <id>_http_<method>_<slug>.json   (method ∈ get|post|put|patch|delete|head|options)
//   gRPC:  <id>_grpc_<slug>.json            (NO method segment — UI does not show it)
// id: [a-z0-9]+ (does NOT contain '_'), unique/stable; NOT shown in UI.
// slug may contain both '_' and '-'; parser splits the first token = id, the rest by type.
// BACK-COMPAT: old files without id (first token is 'http'/'grpc') -> still parse, id empty (migrate later).
#pragma once

#include <string>

#include "core/types.hpp"

namespace core {

// Filename parse result. ok=false => grammar mismatch (call content-reading fallback, §5).
struct ParsedRequestName {
    bool ok = false;
    std::string id;       // first token (empty if old file without id)
    RequestType type = RequestType::Http;
    std::string method;   // HTTP only; empty for gRPC
    std::string slug;     // name-of-request part, keeps inner '_' and '-'
};

// Parse "<id>_http_get_slug[.json]" / "<id>_grpc_slug[.json]" (and old no-id form). .json optional.
ParsedRequestName parseRequestFilename(const std::string& filename);

// Build a filename from (id, type, method, display name). method ignored for gRPC.
// -> "<id>_http_<method>_<slug>.json" or "<id>_grpc_<slug>.json".
std::string encodeRequestFilename(const std::string& id, RequestType type,
                                  const std::string& method, const std::string& displayName);

// True if the string is valid as an id in a FILENAME (only [a-z0-9], non-empty, no '_').
bool isValidFileId(const std::string& id);

// slug -> clean display name: '_'/'-' to spaces, drop special chars,
// collapse spaces, lowercase, sentence-case (capitalize first letter). E.g. "get-list-user" -> "Get list user".
std::string normalizeDisplayName(const std::string& slug);

} // namespace core
