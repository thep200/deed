#include "infra/cache/cache_codec.hpp"

#include <nlohmann/json.hpp>

namespace core::cachecodec {

namespace {
using json = nlohmann::json;
namespace d = core::domain;

std::string getStr(const json& j, const char* k, const std::string& def = "") {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
int getInt(const json& j, const char* k, int def = 0) {
    auto it = j.find(k);
    return (it != j.end() && it->is_number_integer()) ? it->get<int>() : def;
}
bool getBool(const json& j, const char* k, bool def = false) {
    auto it = j.find(k);
    if (it == j.end()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    return def;
}

json headersToJson(const std::vector<d::ResponseHeader>& v) {
    json a = json::array();
    for (const auto& h : v) a.push_back({{"key", h.name}, {"value", h.value}});
    return a;
}
std::vector<d::ResponseHeader> headersFrom(const json& j) {
    std::vector<d::ResponseHeader> out;
    if (!j.is_array()) return out;
    for (const auto& e : j)
        if (e.is_object()) out.push_back({getStr(e, "key"), getStr(e, "value")});
    return out;
}
json cookiesToJson(const std::vector<d::Cookie>& v) {
    json a = json::array();
    for (const auto& c : v)
        a.push_back({{"name", c.name}, {"value", c.value}, {"domain", c.domain},
                     {"path", c.path}, {"expires", c.expires}});
    return a;
}
std::vector<d::Cookie> cookiesFrom(const json& j) {
    std::vector<d::Cookie> out;
    if (!j.is_array()) return out;
    for (const auto& e : j)
        if (e.is_object())
            out.push_back({getStr(e, "name"), getStr(e, "value"), getStr(e, "domain"),
                           getStr(e, "path"), getStr(e, "expires")});
    return out;
}

json apiResponseToJson(const d::ApiResponse& r) {
    return json{{"statusCode", r.statusCode},
                {"headers", headersToJson(r.headers)},
                {"cookies", cookiesToJson(r.cookies)},
                {"body", r.body},
                {"elapsedMs", static_cast<long long>(r.elapsed.count())}};
}
d::ApiResponse apiResponseFrom(const json& j) {
    d::ApiResponse r;
    if (!j.is_object()) return r;
    r.statusCode = getInt(j, "statusCode", 0);
    if (auto it = j.find("headers"); it != j.end()) r.headers = headersFrom(*it);
    if (auto it = j.find("cookies"); it != j.end()) r.cookies = cookiesFrom(*it);
    r.body = getStr(j, "body");
    if (auto it = j.find("elapsedMs"); it != j.end() && it->is_number())
        r.elapsed = std::chrono::milliseconds(it->get<long long>());
    return r;
}

d::ErrorKind errorKindFrom(int v) {
    // domain ErrorKind: Network..Internal (0..7). Clamp unknown to Internal.
    if (v < 0 || v > static_cast<int>(d::ErrorKind::Internal)) return d::ErrorKind::Internal;
    return static_cast<d::ErrorKind>(v);
}

} // namespace

std::string toJson(const ResponseRecord& rec) {
    json j{{"isError", rec.isError},
           {"errorKind", static_cast<int>(rec.errorKind)},
           {"errorMessage", rec.errorMessage},
           {"response", apiResponseToJson(rec.response)},
           {"receivedAt", static_cast<long long>(rec.receivedAt)},
           {"requestRevision", rec.requestRevision},
           {"bytes", static_cast<long long>(rec.bytes)}};
    return j.dump();
}

ResponseRecord fromJson(const std::string& text) {
    json j = json::parse(text);
    ResponseRecord rec;
    if (!j.is_object()) return rec;
    rec.isError = getBool(j, "isError", false);
    rec.errorKind = errorKindFrom(getInt(j, "errorKind", static_cast<int>(core::domain::ErrorKind::Internal)));
    rec.errorMessage = getStr(j, "errorMessage");
    if (auto it = j.find("response"); it != j.end()) rec.response = apiResponseFrom(*it);
    if (auto it = j.find("receivedAt"); it != j.end() && it->is_number())
        rec.receivedAt = it->get<std::int64_t>();
    rec.requestRevision = getStr(j, "requestRevision");
    if (auto it = j.find("bytes"); it != j.end() && it->is_number())
        rec.bytes = it->get<std::uint64_t>();
    return rec;
}

} // namespace core::cachecodec
