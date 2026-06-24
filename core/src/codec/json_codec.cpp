#include "codec/json_codec.hpp"

namespace core::codec {

namespace {

// ---- safe helpers (missing key -> default) ----
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
    if (it->is_number()) return it->get<double>() != 0;   // accept 0/1 (new format)
    return def;
}

json kvArray(const std::vector<KeyValue>& v) {
    json a = json::array();
    for (const auto& kv : v) {
        a.push_back({{"key", kv.key}, {"value", kv.value}, {"enabled", kv.enabled ? 1 : 0}});
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
                         {"filePath", p.filePath}, {"enabled", p.enabled ? 1 : 0}});
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
    if (h.streamMode != HttpStreamMode::None)
        j["streamMode"] = (h.streamMode == HttpStreamMode::Sse) ? "sse" : "auto";
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
    std::string sm = getStr(j, "streamMode", "none");
    h.streamMode = (sm == "sse") ? HttpStreamMode::Sse
                 : (sm == "auto") ? HttpStreamMode::Auto
                                  : HttpStreamMode::None;
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

json wsToJson(const WsRequest& w) {
    json j;
    j["url"] = w.url;
    j["headers"] = kvArray(w.headers);
    j["subprotocols"] = w.subprotocols;
    j["onOpenSend"] = w.onOpenSend;
    j["defaultSendKind"] = (w.defaultSendKind == WsSendKind::Binary) ? "binary" : "text";
    return j;
}

WsRequest wsFrom(const json& j) {
    WsRequest w;
    if (!j.is_object()) return w;
    w.url = getStr(j, "url");
    if (auto it = j.find("headers"); it != j.end()) w.headers = kvFrom(*it);
    if (auto it = j.find("subprotocols"); it != j.end() && it->is_array())
        for (const auto& e : *it) if (e.is_string()) w.subprotocols.push_back(e.get<std::string>());
    if (auto it = j.find("onOpenSend"); it != j.end() && it->is_array())
        for (const auto& e : *it) if (e.is_string()) w.onOpenSend.push_back(e.get<std::string>());
    w.defaultSendKind = (getStr(j, "defaultSendKind", "text") == "binary") ? WsSendKind::Binary
                                                                           : WsSendKind::Text;
    return w;
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
    else if (m.type == RequestType::WebSocket) j["ws"] = wsToJson(m.ws);
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
    } else if (m.type == RequestType::WebSocket) {
        if (auto it = j.find("ws"); it != j.end()) m.ws = wsFrom(*it);
    } else {
        if (auto it = j.find("grpc"); it != j.end()) m.grpc = grpcFrom(*it);
    }
    return m;
}

std::string dumpRequest(const RequestModel& m) { return toJson(m).dump(2); }

json toJson(const Environment& e) {
    json keys = json::array();
    for (const auto& k : e.keys) {
        keys.push_back(json{{"key", k.key}, {"value", k.value}, {"enabled", k.enabled ? 1 : 0}});
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
            ek.value = getStr(k, "value");   // old env files may lack value if it was once a secret;
                                             // migrateLegacySecrets() already merged the value back in earlier.
            ek.enabled = getBool(k, "enabled", true);
            e.keys.push_back(ek);
        }
    }
    return e;
}

// snake_case keys (app config). cacheResponses/cachePersist NOT exposed to user -> always default true.
json toJson(const AppConfig& c) {
    return json{{"default_timeout_ms", c.defaultTimeoutMs},
                {"verify_tls", c.verifyTls},
                {"last_collection_root", c.lastCollectionRoot},
                {"font_name", c.fontName},
                {"font_size", c.fontSize},
                {"ram_cache_size", c.ramCacheSizeMb},
                {"disk_cache_size", c.diskCacheSizeMb}};
}
namespace {
// Read int by snake_case key, fall back to the old camelCase key (compat with old config.json).
int getIntCompat(const json& j, const char* snake, const char* camel, int def) {
    if (j.find(snake) != j.end()) return getInt(j, snake, def);
    return getInt(j, camel, def);
}
std::string getStrCompat(const json& j, const char* snake, const char* camel,
                         const std::string& def = "") {
    if (j.find(snake) != j.end()) return getStr(j, snake);
    if (j.find(camel) != j.end()) return getStr(j, camel);
    return def;
}
bool getBoolCompat(const json& j, const char* snake, const char* camel, bool def) {
    if (j.find(snake) != j.end()) return getBool(j, snake, def);
    return getBool(j, camel, def);
}
} // namespace
AppConfig appConfigFromJson(const json& j) { return appConfigFromJson(j, AppConfig{}); }

AppConfig appConfigFromJson(const json& j, const AppConfig& def) {
    AppConfig c = def;   // missing key -> keep the default value (from .env)
    c.defaultTimeoutMs = getIntCompat(j, "default_timeout_ms", "defaultTimeoutMs", def.defaultTimeoutMs);
    c.verifyTls = getBoolCompat(j, "verify_tls", "verifyTls", def.verifyTls);
    c.lastCollectionRoot = getStrCompat(j, "last_collection_root", "lastCollectionRoot", def.lastCollectionRoot);
    c.fontName = getStrCompat(j, "font_name", "fontName", def.fontName);
    c.fontSize = getIntCompat(j, "font_size", "fontSize", def.fontSize);
    c.ramCacheSizeMb = getIntCompat(j, "ram_cache_size", "ramCacheSizeMb", def.ramCacheSizeMb);
    c.diskCacheSizeMb = getIntCompat(j, "disk_cache_size", "diskCacheSizeMb", def.diskCacheSizeMb);
    // cacheResponses/cachePersist no longer in user config -> keep the struct's default true.
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

// ---- ResponseRecord (disk cache) ----
namespace {
json cookieArray(const std::vector<Cookie>& v) {
    json a = json::array();
    for (const auto& c : v)
        a.push_back({{"name", c.name}, {"value", c.value}, {"domain", c.domain},
                     {"path", c.path}, {"expires", c.expires}});
    return a;
}
std::vector<Cookie> cookieFrom(const json& j) {
    std::vector<Cookie> out;
    if (!j.is_array()) return out;
    for (const auto& e : j)
        if (e.is_object())
            out.push_back({getStr(e, "name"), getStr(e, "value"), getStr(e, "domain"),
                           getStr(e, "path"), getStr(e, "expires")});
    return out;
}
json apiResponseToJson(const ApiResponse& r) {
    return json{{"statusCode", r.statusCode}, {"statusText", r.statusText},
                {"headers", kvArray(r.headers)}, {"cookies", cookieArray(r.cookies)},
                {"body", r.body}, {"elapsedMs", static_cast<long long>(r.elapsedMs)},
                {"sizeBytes", static_cast<long long>(r.sizeBytes)},
                {"resolvedRequestDump", r.resolvedRequestDump},
                {"wasStreamed", r.wasStreamed}, {"partial", r.partial},
                {"eventCount", static_cast<long long>(r.eventCount)}};
}
ApiResponse apiResponseFrom(const json& j) {
    ApiResponse r;
    if (!j.is_object()) return r;
    r.statusCode = getInt(j, "statusCode", 0);
    r.statusText = getStr(j, "statusText");
    if (auto it = j.find("headers"); it != j.end()) r.headers = kvFrom(*it);
    if (auto it = j.find("cookies"); it != j.end()) r.cookies = cookieFrom(*it);
    r.body = getStr(j, "body");
    if (auto it = j.find("elapsedMs"); it != j.end() && it->is_number()) r.elapsedMs = it->get<long>();
    if (auto it = j.find("sizeBytes"); it != j.end() && it->is_number())
        r.sizeBytes = it->get<std::int64_t>();
    r.resolvedRequestDump = getStr(j, "resolvedRequestDump");
    r.wasStreamed = getBool(j, "wasStreamed", false);
    r.partial = getBool(j, "partial", false);
    if (auto it = j.find("eventCount"); it != j.end() && it->is_number())
        r.eventCount = it->get<std::uint64_t>();
    return r;
}
} // namespace

json toJson(const ResponseRecord& rec) {
    return json{{"isError", rec.isError},
                {"errorKind", static_cast<int>(rec.errorKind)},
                {"errorMessage", rec.errorMessage},
                {"response", apiResponseToJson(rec.response)},
                {"receivedAt", static_cast<long long>(rec.receivedAt)},
                {"requestRevision", rec.requestRevision},
                {"bytes", static_cast<long long>(rec.bytes)}};
}
ResponseRecord responseRecordFromJson(const json& j) {
    ResponseRecord rec;
    if (!j.is_object()) return rec;
    rec.isError = getBool(j, "isError", false);
    rec.errorKind = static_cast<ErrorKind>(getInt(j, "errorKind", static_cast<int>(ErrorKind::Unknown)));
    rec.errorMessage = getStr(j, "errorMessage");
    if (auto it = j.find("response"); it != j.end()) rec.response = apiResponseFrom(*it);
    if (auto it = j.find("receivedAt"); it != j.end() && it->is_number())
        rec.receivedAt = it->get<std::int64_t>();
    rec.requestRevision = getStr(j, "requestRevision");
    if (auto it = j.find("bytes"); it != j.end() && it->is_number())
        rec.bytes = it->get<std::uint64_t>();
    return rec;
}

} // namespace core::codec
