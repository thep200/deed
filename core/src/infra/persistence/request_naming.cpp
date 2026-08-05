#include "core/infra/persistence/request_naming.hpp"

#include <cctype>

#include "core/infra/persistence/order_key.hpp"
#include "infra/platform/fs_util.hpp"

namespace core {

namespace {

// Separator between the order key and the rest of the name. MUST sort below every base62 digit
// ('+' = 43 < '0' = 48) so a shorter key still compares before the keys that extend it.
constexpr char kOrderSep = '+';

// Strip a trailing ".json" extension (case-insensitive) if present.
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
// Parse the "http_<method>_<slug>" / "grpc_<slug>" part (after the id is split off). Returns ok + fills type/method/slug.
bool parseTypeRest(const std::string& rest, ParsedRequestName& out) {
    std::size_t sep = rest.find('_');
    if (sep == std::string::npos) return false;
    std::string type = rest.substr(0, sep);
    if (type == "grpc" || type == "ws" || type == "gql" || type == "kafka" || type == "soap") {
        out.type = (type == "ws")   ? RequestType::WebSocket
                 : (type == "gql")  ? RequestType::GraphQl
                 : (type == "kafka") ? RequestType::Kafka
                 : (type == "soap")  ? RequestType::Soap
                                     : RequestType::Grpc;
        out.slug = rest.substr(sep + 1);            // everything left = slug
        out.method.clear();
        return !out.slug.empty();
    }
    if (type == "http") {
        std::size_t methodSep = rest.find('_', sep + 1);   // [http, method, slugRest]
        if (methodSep == std::string::npos) return false;
        out.type = RequestType::Http;
        out.method = rest.substr(sep + 1, methodSep - (sep + 1));
        out.slug = rest.substr(methodSep + 1);      // slug keeps its '_'
        return !out.method.empty() && !out.slug.empty();
    }
    return false;
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
    if (type == RequestType::Grpc) return prefix + "grpc_" + slug + ".json";   // NO method
    if (type == RequestType::WebSocket) return prefix + "ws_" + slug + ".json"; // NO method
    if (type == RequestType::GraphQl) return prefix + "gql_" + slug + ".json";  // NO method
    if (type == RequestType::Kafka) return prefix + "kafka_" + slug + ".json";  // NO method
    if (type == RequestType::Soap) return prefix + "soap_" + slug + ".json";    // NO method
    std::string m;
    for (unsigned char c : method) m += static_cast<char>(std::tolower(c));
    if (m.empty()) m = "get";
    return prefix + "http_" + m + "_" + slug + ".json";
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
    while (!s.empty() && s.back() == ' ') s.pop_back();   // trim trailing
    if (!s.empty()) s[0] = static_cast<char>(std::toupper((unsigned char)s[0]));  // sentence-case
    return s;
}

} // namespace core
