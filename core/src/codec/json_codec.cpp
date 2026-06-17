#include "codec/json_codec.hpp"

namespace core::codec {

namespace {

// ---- helpers an toàn (key thiếu -> mặc định) ----
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
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

json kvArray(const std::vector<KeyValue>& v) {
    json a = json::array();
    for (const auto& kv : v) {
        a.push_back({{"key", kv.key}, {"value", kv.value}, {"enabled", kv.enabled}});
    }
    return a;
}
std::vector<KeyValue> kvFrom(const json& j) {
    std::vector<KeyValue> out;
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        out.push_back({getStr(e, "key"), getStr(e, "value"), getBool(e, "enabled", true)});
    }
    return out;
}

json bodyToJson(const Body& b) {
    json j;
    j["mode"] = b.mode;
    if (b.mode == "json") j["json"] = b.json;
    else if (b.mode == "text") j["text"] = b.text;
    else if (b.mode == "xml") j["xml"] = b.xml;
    else if (b.mode == "form-urlencoded") j["formUrlEncoded"] = kvArray(b.formUrlEncoded);
    else if (b.mode == "multipart") {
        json a = json::array();
        for (const auto& p : b.multipart) {
            a.push_back({{"key", p.key}, {"value", p.value}, {"type", p.type},
                         {"filePath", p.filePath}, {"enabled", p.enabled}});
        }
        j["multipart"] = a;
    } else if (b.mode == "binary") {
        j["binary"] = {{"filePath", b.binaryFilePath}};
    } else if (b.mode == "graphql") {
        j["graphql"] = {{"query", b.graphqlQuery}, {"variables", b.graphqlVariables}};
    }
    return j;
}

Body bodyFrom(const json& j) {
    Body b;
    if (!j.is_object()) return b;
    b.mode = getStr(j, "mode", "none");
    b.json = getStr(j, "json");
    b.text = getStr(j, "text");
    b.xml = getStr(j, "xml");
    if (auto it = j.find("formUrlEncoded"); it != j.end()) b.formUrlEncoded = kvFrom(*it);
    if (auto it = j.find("multipart"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            MultipartPart p;
            p.key = getStr(e, "key");
            p.value = getStr(e, "value");
            p.type = getStr(e, "type", "text");
            p.filePath = getStr(e, "filePath");
            p.enabled = getBool(e, "enabled", true);
            b.multipart.push_back(p);
        }
    }
    if (auto it = j.find("binary"); it != j.end() && it->is_object())
        b.binaryFilePath = getStr(*it, "filePath");
    if (auto it = j.find("graphql"); it != j.end() && it->is_object()) {
        b.graphqlQuery = getStr(*it, "query");
        b.graphqlVariables = getStr(*it, "variables");
    }
    return b;
}

json authToJson(const Auth& a) {
    json j;
    j["type"] = a.type;
    if (a.type == "basic") j["basic"] = {{"username", a.basicUsername}, {"password", a.basicPassword}};
    else if (a.type == "bearer") j["bearer"] = {{"token", a.bearerToken}};
    else if (a.type == "apikey")
        j["apikey"] = {{"key", a.apikeyKey}, {"value", a.apikeyValue}, {"in", a.apikeyIn}};
    return j;
}

Auth authFrom(const json& j) {
    Auth a;
    if (!j.is_object()) return a;
    a.type = getStr(j, "type", "none");
    if (auto it = j.find("basic"); it != j.end() && it->is_object()) {
        a.basicUsername = getStr(*it, "username");
        a.basicPassword = getStr(*it, "password");
    }
    if (auto it = j.find("bearer"); it != j.end() && it->is_object())
        a.bearerToken = getStr(*it, "token");
    if (auto it = j.find("apikey"); it != j.end() && it->is_object()) {
        a.apikeyKey = getStr(*it, "key");
        a.apikeyValue = getStr(*it, "value");
        a.apikeyIn = getStr(*it, "in", "header");
    }
    return a;
}

json httpToJson(const HttpRequest& h) {
    json j;
    j["method"] = h.method;
    j["url"] = h.url;
    j["pathVariables"] = kvArray(h.pathVariables);
    j["params"] = kvArray(h.params);
    j["headers"] = kvArray(h.headers);
    j["body"] = bodyToJson(h.body);
    j["auth"] = authToJson(h.auth);
    json s;
    if (h.settings.timeoutMsSet) s["timeoutMs"] = h.settings.timeoutMs;
    if (h.settings.followRedirectsSet) s["followRedirects"] = h.settings.followRedirects;
    if (h.settings.verifyTlsSet) s["verifyTls"] = h.settings.verifyTls;
    if (!s.empty()) j["settings"] = s;
    return j;
}

HttpRequest httpFrom(const json& j) {
    HttpRequest h;
    if (!j.is_object()) return h;
    h.method = getStr(j, "method", "GET");
    h.url = getStr(j, "url");
    if (auto it = j.find("pathVariables"); it != j.end()) h.pathVariables = kvFrom(*it);
    if (auto it = j.find("params"); it != j.end()) h.params = kvFrom(*it);
    if (auto it = j.find("headers"); it != j.end()) h.headers = kvFrom(*it);
    if (auto it = j.find("body"); it != j.end()) h.body = bodyFrom(*it);
    if (auto it = j.find("auth"); it != j.end()) h.auth = authFrom(*it);
    if (auto it = j.find("settings"); it != j.end() && it->is_object()) {
        const auto& s = *it;
        if (s.contains("timeoutMs")) { h.settings.timeoutMs = getInt(s, "timeoutMs", 30000); h.settings.timeoutMsSet = true; }
        if (s.contains("followRedirects")) { h.settings.followRedirects = getBool(s, "followRedirects", true); h.settings.followRedirectsSet = true; }
        if (s.contains("verifyTls")) { h.settings.verifyTls = getBool(s, "verifyTls", true); h.settings.verifyTlsSet = true; }
    }
    return h;
}

json grpcToJson(const GrpcRequest& g) {
    json j;
    j["target"] = g.target;
    j["service"] = g.service;
    j["method"] = g.method;
    j["methodType"] = g.methodType;
    json ps;
    ps["mode"] = g.protoSource.mode;
    if (g.protoSource.mode == "protoFiles") {
        ps["files"] = g.protoSource.files;
        ps["importPaths"] = g.protoSource.importPaths;
    } else if (g.protoSource.mode == "descriptorSet") {
        ps["path"] = g.protoSource.descriptorSetPath;
    }
    j["protoSource"] = ps;
    j["message"] = g.message;
    j["metadata"] = kvArray(g.metadata);
    j["tls"] = {{"enabled", g.tls.enabled},
                {"insecureSkipVerify", g.tls.insecureSkipVerify},
                {"caCertPath", g.tls.caCertPath},
                {"clientCertPath", g.tls.clientCertPath},
                {"clientKeyPath", g.tls.clientKeyPath}};
    json s;
    if (g.settings.deadlineMsSet) s["deadlineMs"] = g.settings.deadlineMs;
    if (!s.empty()) j["settings"] = s;
    return j;
}

GrpcRequest grpcFrom(const json& j) {
    GrpcRequest g;
    if (!j.is_object()) return g;
    g.target = getStr(j, "target");
    g.service = getStr(j, "service");
    g.method = getStr(j, "method");
    g.methodType = getStr(j, "methodType", "unary");
    if (auto it = j.find("protoSource"); it != j.end() && it->is_object()) {
        const auto& ps = *it;
        g.protoSource.mode = getStr(ps, "mode", "reflection");
        if (auto f = ps.find("files"); f != ps.end() && f->is_array())
            for (const auto& e : *f) if (e.is_string()) g.protoSource.files.push_back(e.get<std::string>());
        if (auto f = ps.find("importPaths"); f != ps.end() && f->is_array())
            for (const auto& e : *f) if (e.is_string()) g.protoSource.importPaths.push_back(e.get<std::string>());
        g.protoSource.descriptorSetPath = getStr(ps, "path");
    }
    g.message = getStr(j, "message");
    if (auto it = j.find("metadata"); it != j.end()) g.metadata = kvFrom(*it);
    if (auto it = j.find("tls"); it != j.end() && it->is_object()) {
        const auto& t = *it;
        g.tls.enabled = getBool(t, "enabled");
        g.tls.insecureSkipVerify = getBool(t, "insecureSkipVerify");
        g.tls.caCertPath = getStr(t, "caCertPath");
        g.tls.clientCertPath = getStr(t, "clientCertPath");
        g.tls.clientKeyPath = getStr(t, "clientKeyPath");
    }
    if (auto it = j.find("settings"); it != j.end() && it->is_object()) {
        if (it->contains("deadlineMs")) { g.settings.deadlineMs = getInt(*it, "deadlineMs", 30000); g.settings.deadlineMsSet = true; }
    }
    return g;
}

} // namespace

json toJson(const RequestModel& m) {
    json j;
    j["schemaVersion"] = m.schemaVersion;
    if (!m.id.empty()) j["id"] = m.id;
    j["name"] = m.name;
    if (!m.description.empty()) j["description"] = m.description;
    j["type"] = toString(m.type);
    j["seq"] = m.seq;
    if (m.type == RequestType::Http) j["http"] = httpToJson(m.http);
    else j["grpc"] = grpcToJson(m.grpc);
    return j;
}

RequestModel requestFromJson(const json& j) {
    RequestModel m;
    m.schemaVersion = getInt(j, "schemaVersion", 1);
    m.id = getStr(j, "id");
    m.name = getStr(j, "name");
    m.description = getStr(j, "description");
    std::string t = getStr(j, "type", "http");
    parseRequestType(t, m.type);
    m.seq = getInt(j, "seq", 0);
    if (m.type == RequestType::Http) {
        if (auto it = j.find("http"); it != j.end()) m.http = httpFrom(*it);
    } else {
        if (auto it = j.find("grpc"); it != j.end()) m.grpc = grpcFrom(*it);
    }
    return m;
}

std::string dumpRequest(const RequestModel& m) { return toJson(m).dump(2); }

json toJson(const Environment& e) {
    json keys = json::array();
    for (const auto& k : e.keys) {
        json kj = {{"key", k.key}, {"secret", k.secret}, {"enabled", k.enabled}};
        if (!k.secret) kj["value"] = k.value; // secret -> không ghi value vào file env
        keys.push_back(kj);
    }
    return json{{"schemaVersion", e.schemaVersion}, {"name", e.name}, {"keys", keys}};
}

Environment envFromJson(const json& j) {
    Environment e;
    e.schemaVersion = getInt(j, "schemaVersion", 1);
    e.name = getStr(j, "name");
    if (auto it = j.find("keys"); it != j.end() && it->is_array()) {
        for (const auto& k : *it) {
            EnvKey ek;
            ek.key = getStr(k, "key");
            ek.value = getStr(k, "value");
            ek.secret = getBool(k, "secret");
            ek.enabled = getBool(k, "enabled", true);
            e.keys.push_back(ek);
        }
    }
    return e;
}

json toJson(const AppConfig& c) {
    return json{{"defaultTimeoutMs", c.defaultTimeoutMs},
                {"verifyTls", c.verifyTls},
                {"lastCollectionRoot", c.lastCollectionRoot},
                {"fontName", c.fontName},
                {"fontSize", c.fontSize}};
}
AppConfig appConfigFromJson(const json& j) {
    AppConfig c;
    c.defaultTimeoutMs = getInt(j, "defaultTimeoutMs", 30000);
    c.verifyTls = getBool(j, "verifyTls", true);
    c.lastCollectionRoot = getStr(j, "lastCollectionRoot");
    c.fontName = getStr(j, "fontName");
    c.fontSize = getInt(j, "fontSize", 11);
    return c;
}

json toJson(const Session& s) {
    return json{{"schemaVersion", s.schemaVersion},
                {"lastOpenedFile", s.lastOpenedFile},
                {"activeEnv", s.activeEnv}};
}
Session sessionFromJson(const json& j) {
    Session s;
    s.schemaVersion = getInt(j, "schemaVersion", 1);
    s.lastOpenedFile = getStr(j, "lastOpenedFile");
    s.activeEnv = getStr(j, "activeEnv", "Global");
    return s;
}

} // namespace core::codec
