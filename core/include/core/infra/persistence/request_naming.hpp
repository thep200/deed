// The filename is a DERIVED CACHE of the content: the tree + id are built WITHOUT reading the file.
// '+' sorts below every base62 digit, so a byte compare of filenames IS the sibling order; folders use "<order>+<slug>".
// No migration path: a file missing id or order came from outside the app.
#pragma once

#include <string>

#include "core/domain/request/request_type.hpp"

namespace core {

// ok=false => not our grammar (foreign file).
struct ParsedRequestName {
    bool ok = false;
    std::string order;    // fractional-index prefix ("" = foreign file)
    std::string id;       // first token after the order prefix
    RequestType type = RequestType::Http;
    std::string method;   // HTTP only; empty for gRPC
    std::string slug;     // name-of-request part, keeps inner '_' and '-'
};

// Parse "[<order>+]<id>_http_get_slug[.json]" / "[<order>+]<id>_grpc_slug[.json]". .json optional.
ParsedRequestName parseRequestFilename(const std::string& filename);

// method ignored for gRPC; an empty order emits no prefix.
std::string encodeRequestFilename(const std::string& id, RequestType type,
                                  const std::string& method, const std::string& displayName,
                                  const std::string& order = "");

// Shared by request files AND folder names (folders have no other grammar).
// splitOrderPrefix("a0+auth") -> {"a0", "auth"}; no valid prefix -> {"", name}.
struct SplitOrder {
    std::string order;
    std::string rest;
};
SplitOrder splitOrderPrefix(const std::string& name);
// Inverse: empty order -> rest unchanged.
std::string withOrderPrefix(const std::string& order, const std::string& rest);

// True if the string is valid as an id in a FILENAME (only [a-z0-9], non-empty, no '_').
bool isValidFileId(const std::string& id);

// '_'/'-' -> spaces, drop special chars, collapse, sentence-case: "get-list-user" -> "Get list user".
std::string normalizeDisplayName(const std::string& slug);

} // namespace core
