#include "core/infra/persistence/request_naming.hpp"

#include <cctype>

#include "core/infra/persistence/order_key.hpp"
#include "infra/platform/fs_util.hpp"

namespace core {

namespace {

// Order-key separator MUST sort below every base62 digit ('+' = 43 < '0' = 48) so a shorter key
// still compares before the keys that extend it.
constexpr char kOrderSep = '+';

std::string stripJsonExt(const std::string& name) {
    const std::string ext = ".json";
    if (name.size() >= ext.size()) {
        std::string tail = name.substr(name.size() - ext.size());
        for (auto& c : tail) c = static_cast<char>(std::tolower((unsigned char)c));
        if (tail == ext) return name.substr(0, name.size() - ext.size());
    }
    return name;
}

} // namespace

namespace {

// GraphQl's file token is "gql" (NOT the wire token "graphql") because existing files on disk use it.
constexpr const char* kFileTokens[] = {"http", "grpc", "gql", "ws", "kafka", "soap", "ldap"};
constexpr bool kHasMethodSegment[] = {true, false, false, false, false, false, false};
static_assert(sizeof(kFileTokens) / sizeof(kFileTokens[0]) == kRequestTypeCount);
static_assert(sizeof(kHasMethodSegment) / sizeof(kHasMethodSegment[0]) == kRequestTypeCount);

const char* fileTokenOf(RequestType t) { return kFileTokens[static_cast<std::size_t>(t)]; }

bool typeFromFileToken(const std::string& token, RequestType& out) {
    for (std::size_t i = 0; i < kRequestTypeCount; ++i)
        if (token == kFileTokens[i]) {
            out = static_cast<RequestType>(i);
            return true;
        }
    return false;
}

// Parse the "http_<method>_<slug>" / "<type>_<slug>" part (after the id is split off).
bool parseTypeRest(const std::string& rest, ParsedRequestName& out) {
    std::size_t sep = rest.find('_');
    if (sep == std::string::npos) return false;
    RequestType t;
    if (!typeFromFileToken(rest.substr(0, sep), t)) return false;
    out.type = t;
    if (kHasMethodSegment[static_cast<std::size_t>(t)]) {
        std::size_t methodSep = rest.find('_', sep + 1);   // [type, method, slugRest]
        if (methodSep == std::string::npos) return false;
        out.method = rest.substr(sep + 1, methodSep - (sep + 1));
        out.slug = rest.substr(methodSep + 1);      // slug keeps its '_'
        return !out.method.empty() && !out.slug.empty();
    }
    out.method.clear();
    out.slug = rest.substr(sep + 1);
    return !out.slug.empty();
}
} // namespace

bool isValidFileId(const std::string& id) {
    if (id.empty()) return false;
    for (unsigned char c : id)
        if (!std::islower(c) && !std::isdigit(c)) return false;   // only [a-z0-9]
    return true;
}

SplitOrder splitOrderPrefix(const std::string& name) {
    std::size_t plus = name.find(kOrderSep);
    if (plus == std::string::npos || plus == 0) return {"", name};
    std::string key = name.substr(0, plus);
    if (!orderkey::isValid(key)) return {"", name};   // '+' from somewhere else -> not an order prefix
    return {key, name.substr(plus + 1)};
}

std::string withOrderPrefix(const std::string& order, const std::string& rest) {
    return order.empty() ? rest : order + kOrderSep + rest;
}

ParsedRequestName parseRequestFilename(const std::string& filename) {
    ParsedRequestName out;
    SplitOrder so = splitOrderPrefix(stripJsonExt(filename));
    out.order = so.order;
    const std::string& base = so.rest;

    std::size_t firstSep = base.find('_');
    if (firstSep == std::string::npos) return out;   // bad grammar -> ok=false

    out.id = base.substr(0, firstSep);               // first token = id
    std::string rest = base.substr(firstSep + 1);
    out.ok = isValidFileId(out.id) && parseTypeRest(rest, out);
    return out;
}

std::string encodeRequestFilename(const std::string& id, RequestType type,
                                  const std::string& method, const std::string& displayName,
                                  const std::string& order) {
    std::string slug = fsutil::slugify(displayName);   // [a-z0-9-], '-' replaces spaces
    std::string prefix = withOrderPrefix(order, id + "_");   // [order+]id, id always before the type
    std::string token = fileTokenOf(type);
    if (!kHasMethodSegment[static_cast<std::size_t>(type)])
        return prefix + token + "_" + slug + ".json";
    std::string m;
    for (unsigned char c : method) m += static_cast<char>(std::tolower(c));
    if (m.empty()) m = "get";
    return prefix + token + "_" + m + "_" + slug + ".json";
}

std::string normalizeDisplayName(const std::string& slug) {
    std::string s;
    s.reserve(slug.size());
    bool prevSpace = false;
    for (unsigned char c : slug) {
        if (c == '_' || c == '-' || std::isspace(c)) {
            if (!prevSpace && !s.empty()) { s += ' '; prevSpace = true; }
        } else if (std::isalnum(c)) {
            s += static_cast<char>(std::tolower(c));
            prevSpace = false;
        }
        // other special chars -> drop
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (!s.empty()) s[0] = static_cast<char>(std::toupper((unsigned char)s[0]));  // sentence-case
    return s;
}

} // namespace core
