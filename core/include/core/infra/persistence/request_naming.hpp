// request_naming.hpp — request file naming convention (new_format.file.md §2A; updated LAZY_TREE §2).
// The filename is a DERIVED CACHE of the content: lets us build the tree + get id WITHOUT reading the file.
//   HTTP:  [<order>+]<id>_http_<method>_<slug>.json  (method ∈ get|post|put|patch|delete|head|options)
//   gRPC:  [<order>+]<id>_grpc_<slug>.json           (NO method segment — UI does not show it)
// order: fractional-index key (order_key.hpp) = sibling order. '+' sorts below every digit, so a
//   byte compare of filenames IS the order. Folders use "<order>+<slug>".
// id: [a-z0-9]+ (no '_'), unique/stable; NOT shown in UI.
// slug may contain '_' and '-'; parser splits the first token = id, the rest by type.
// App always writes id + order. No migration path: a file missing either came from outside the app.
#pragma once

#include <string>

#include "core/domain/request/request_type.hpp" // RequestType (survives types.hpp removal)

namespace core {

// Filename parse result. ok=false => not our grammar (foreign file).
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

// Build a filename from (order, id, type, method, display name). method ignored for gRPC; an empty
// order emits no prefix. -> "[<order>+]<id>_http_<method>_<slug>.json".
std::string encodeRequestFilename(const std::string& id, RequestType type,
                                  const std::string& method, const std::string& displayName,
                                  const std::string& order = "");

// Order prefix helpers — shared by request files AND folder names (folders have no other grammar).
// splitOrderPrefix("a0+auth") -> {"a0", "auth"};  no valid prefix -> {"", name}.
struct SplitOrder {
    std::string order;
    std::string rest;
};
SplitOrder splitOrderPrefix(const std::string& name);
// Inverse: empty order -> rest unchanged.
std::string withOrderPrefix(const std::string& order, const std::string& rest);

// True if the string is valid as an id in a FILENAME (only [a-z0-9], non-empty, no '_').
bool isValidFileId(const std::string& id);

// slug -> clean display name: '_'/'-' to spaces, drop special chars,
// collapse spaces, lowercase, sentence-case (capitalize first letter). E.g. "get-list-user" -> "Get list user".
std::string normalizeDisplayName(const std::string& slug);

} // namespace core
