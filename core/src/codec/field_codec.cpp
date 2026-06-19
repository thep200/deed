#include "core/codec/field_codec.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace core::fieldcodec {

namespace {
std::string gs(const json& j, const char* k, const std::string& d = "") {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
}
bool gb(const json& j, const char* k, bool d = false) {
    auto it = j.find(k);
    if (it == j.end()) return d;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;   // chấp nhận 0/1 (dạng mới)
    return d;
}
} // namespace

std::string keyValuesToJson(const std::vector<KeyValue>& kvs) {
    json a = json::array();
    for (const auto& kv : kvs)
        a.push_back({{"key", kv.key}, {"value", kv.value}, {"enabled", kv.enabled ? 1 : 0}});
    return a.dump(2);
}

bool jsonToKeyValues(const std::string& text, std::vector<KeyValue>& out, std::string& err) {
    try {
        auto j = json::parse(text.empty() ? "[]" : text);
        if (!j.is_array()) { err = "must be a JSON array"; return false; }
        out.clear();
        for (const auto& e : j) {
            if (!e.is_object()) continue;
            out.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
        }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

std::string bodyToJson(const Body& b) {
    json j;
    j["mode"] = b.mode;
    if (b.mode == "json") j["json"] = b.json;
    else if (b.mode == "text") j["text"] = b.text;
    else if (b.mode == "xml") j["xml"] = b.xml;
    else if (b.mode == "form-urlencoded") {
        json a = json::array();
        for (const auto& kv : b.formUrlEncoded)
            a.push_back({{"key", kv.key}, {"value", kv.value}, {"enabled", kv.enabled ? 1 : 0}});
        j["formUrlEncoded"] = a;
    } else if (b.mode == "multipart") {
        json a = json::array();
        for (const auto& p : b.multipart)
            a.push_back({{"key", p.key}, {"value", p.value}, {"type", p.type},
                         {"filePath", p.filePath}, {"enabled", p.enabled ? 1 : 0}});
        j["multipart"] = a;
    } else if (b.mode == "binary") {
        j["binary"] = {{"filePath", b.binaryFilePath}};
    } else if (b.mode == "graphql") {
        j["graphql"] = {{"query", b.graphqlQuery}, {"variables", b.graphqlVariables}};
    }
    return j.dump(2);
}

bool jsonToBody(const std::string& text, Body& b, std::string& err) {
    try {
        auto j = json::parse(text.empty() ? "{\"mode\":\"none\"}" : text);
        if (!j.is_object()) { err = "must be a JSON object"; return false; }
        b = Body{};
        b.mode = gs(j, "mode", "none");
        b.json = gs(j, "json");
        b.text = gs(j, "text");
        b.xml = gs(j, "xml");
        if (auto it = j.find("formUrlEncoded"); it != j.end() && it->is_array())
            for (const auto& e : *it)
                b.formUrlEncoded.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
        if (auto it = j.find("multipart"); it != j.end() && it->is_array())
            for (const auto& e : *it) {
                MultipartPart p;
                p.key = gs(e, "key"); p.value = gs(e, "value");
                p.type = gs(e, "type", "text"); p.filePath = gs(e, "filePath");
                p.enabled = gb(e, "enabled", true);
                b.multipart.push_back(p);
            }
        if (auto it = j.find("binary"); it != j.end() && it->is_object())
            b.binaryFilePath = gs(*it, "filePath");
        if (auto it = j.find("graphql"); it != j.end() && it->is_object()) {
            b.graphqlQuery = gs(*it, "query");
            b.graphqlVariables = gs(*it, "variables");
        }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

std::string authToJson(const Auth& a) {
    json j;
    j["type"] = a.type;
    if (a.type == "basic") j["basic"] = {{"username", a.basicUsername}, {"password", a.basicPassword}};
    else if (a.type == "bearer") j["bearer"] = {{"token", a.bearerToken}};
    else if (a.type == "apikey")
        j["apikey"] = {{"key", a.apikeyKey}, {"value", a.apikeyValue}, {"in", a.apikeyIn}};
    return j.dump(2);
}

bool jsonToAuth(const std::string& text, Auth& a, std::string& err) {
    try {
        auto j = json::parse(text.empty() ? "{\"type\":\"none\"}" : text);
        if (!j.is_object()) { err = "must be a JSON object"; return false; }
        a = Auth{};
        a.type = gs(j, "type", "none");
        if (auto it = j.find("basic"); it != j.end() && it->is_object()) {
            a.basicUsername = gs(*it, "username"); a.basicPassword = gs(*it, "password");
        }
        if (auto it = j.find("bearer"); it != j.end() && it->is_object())
            a.bearerToken = gs(*it, "token");
        if (auto it = j.find("apikey"); it != j.end() && it->is_object()) {
            a.apikeyKey = gs(*it, "key"); a.apikeyValue = gs(*it, "value");
            a.apikeyIn = gs(*it, "in", "header");
        }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

std::string formatJson(const std::string& text, bool pretty) {
    try {
        auto j = json::parse(text);
        return pretty ? j.dump(2) : j.dump();
    } catch (...) {
        return text; // không phải JSON -> giữ nguyên
    }
}

std::string jsonEncodeString(const std::string& text) {
    json j = text;       // string -> "\"...\"" (escape \n, \" ...)
    return j.dump();
}

std::string jsonDecodeString(const std::string& text) {
    try {
        auto j = json::parse(text);
        if (j.is_string()) return j.get<std::string>();
        return text;
    } catch (...) {
        return text;
    }
}

} // namespace core::fieldcodec
