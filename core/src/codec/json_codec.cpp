#include "codec/json_codec.hpp"

#include <stdexcept>
#include <string>

namespace core::codec {

json parseGuarded(const std::string& text, int maxDepth) {
    // O(n) structural-depth pre-scan: count [ and { nesting OUTSIDE of string literals. Reject before the
    // recursive parser can blow the stack (H5). String/escape handling so brackets inside strings don't count.
    int depth = 0;
    bool inStr = false, esc = false;
    for (char c : text) {
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '[' || c == '{') {
            // std::runtime_error derives from std::exception, same as json::parse_error — every existing
            // catch(const std::exception&)/catch(...) around these parse sites handles it identically.
            if (++depth > maxDepth)
                throw std::runtime_error("JSON nesting too deep (max " + std::to_string(maxDepth) + ")");
        } else if (c == ']' || c == '}') {
            if (depth > 0) --depth;
        }
    }
    return json::parse(text);
}

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
    // Persist EVERY field that holds content — core::Body is a tagged union that keeps all body types at
    // once and `mode` only marks the ENABLED one. Writing just the active field would drop the others on
    // save (e.g. saving in "json" mode would erase a filled form), so switching mode after a reload would
    // show an empty default. Symmetric with bodyFrom, which already reads every field back regardless of mode.
    if (!b.json.empty()) j["json"] = b.json;
    if (!b.text.empty()) j["text"] = b.text;
    if (!b.xml.empty()) j["xml"] = b.xml;
    if (!b.formUrlEncoded.empty()) j["formUrlEncoded"] = kvArray(b.formUrlEncoded);
    if (!b.multipart.empty()) {
        json a = json::array();
        for (const auto& p : b.multipart) {
            a.push_back({{"key", p.key}, {"value", p.value}, {"type", p.type},
                         {"filePath", p.filePath}, {"enabled", p.enabled ? 1 : 0}});
        }
        j["multipart"] = a;
    }
    if (!b.binaryFilePath.empty()) j["binary"] = {{"filePath", b.binaryFilePath}};
    if (!b.graphqlQuery.empty() || !b.graphqlVariables.empty())
        j["graphql"] = {{"query", b.graphqlQuery}, {"variables", b.graphqlVariables}};
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
    j["auth"] = authToJson(w.auth);
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
    if (auto it = j.find("auth"); it != j.end()) w.auth = authFrom(*it);
    return w;
}

const char* gqlOpStr(GqlOperation o) {
    switch (o) {
        case GqlOperation::Query: return "query";
        case GqlOperation::Mutation: return "mutation";
        case GqlOperation::Subscription: return "subscription";
        default: return "auto";
    }
}
json gqlToJson(const GraphQlRequest& g) {
    json j;
    j["url"] = g.url;
    j["query"] = g.query;
    j["variables"] = g.variablesJson;
    j["operationName"] = g.operationName;
    j["operation"] = gqlOpStr(g.operation);
    j["subTransport"] = (g.subTransport == GqlSubTransport::Sse) ? "sse" : "ws";
    j["wsProtocol"] = (g.wsProtocol == GqlWsProtocol::SubscriptionsTransportWs)
                          ? "subscriptions-transport-ws" : "graphql-transport-ws";
    if (!g.connectionInitPayloadJson.empty()) j["connectionInitPayload"] = g.connectionInitPayloadJson;
    j["headers"] = kvArray(g.headers);
    if (g.useGetForQuery) j["useGetForQuery"] = true;
    j["auth"] = authToJson(g.auth);
    return j;
}
GraphQlRequest gqlFrom(const json& j) {
    GraphQlRequest g;
    if (!j.is_object()) return g;
    g.url = getStr(j, "url");
    g.query = getStr(j, "query");
    g.variablesJson = getStr(j, "variables", "{}");
    g.operationName = getStr(j, "operationName");
    std::string op = getStr(j, "operation", "auto");
    g.operation = (op == "query") ? GqlOperation::Query
                : (op == "mutation") ? GqlOperation::Mutation
                : (op == "subscription") ? GqlOperation::Subscription : GqlOperation::Auto;
    g.subTransport = (getStr(j, "subTransport", "ws") == "sse") ? GqlSubTransport::Sse
                                                               : GqlSubTransport::WebSocket;
    g.wsProtocol = (getStr(j, "wsProtocol", "graphql-transport-ws") == "subscriptions-transport-ws")
                       ? GqlWsProtocol::SubscriptionsTransportWs : GqlWsProtocol::GraphQlTransportWs;
    g.connectionInitPayloadJson = getStr(j, "connectionInitPayload");
    if (auto it = j.find("headers"); it != j.end()) g.headers = kvFrom(*it);
    g.useGetForQuery = getBool(j, "useGetForQuery", false);
    if (auto it = j.find("auth"); it != j.end()) g.auth = authFrom(*it);
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
    j["config"] = json{{"timeout_ms", m.config.timeoutMs}, {"tls", m.config.tls}};
    if (m.type == RequestType::Http) j["http"] = httpToJson(m.http);
    else if (m.type == RequestType::WebSocket) j["ws"] = wsToJson(m.ws);
    else if (m.type == RequestType::GraphQL) j["graphql"] = gqlToJson(m.graphql);
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
    if (auto it = j.find("config"); it != j.end() && it->is_object()) {
        m.config.timeoutMs = getInt(*it, "timeout_ms", m.config.timeoutMs);
        m.config.tls = getBool(*it, "tls", m.config.tls);
    }
    if (m.type == RequestType::Http) {
        if (auto it = j.find("http"); it != j.end()) m.http = httpFrom(*it);
    } else if (m.type == RequestType::WebSocket) {
        if (auto it = j.find("ws"); it != j.end()) m.ws = wsFrom(*it);
    } else if (m.type == RequestType::GraphQL) {
        if (auto it = j.find("graphql"); it != j.end()) m.graphql = gqlFrom(*it);
    } else {
        if (auto it = j.find("grpc"); it != j.end()) m.grpc = grpcFrom(*it);
    }
    return m;
}

std::string dumpRequest(const RequestModel& m) { return toJson(m).dump(2); }

json toJson(const Environment& e) {
    json keys = json::array();
    for (const auto& k : e.keys) {
        keys.push_back(json{{"key", k.key}, {"value", k.value}, {"enabled", k.enabled ? 1 : 0},
                            {"secret", k.secret ? 1 : 0}});
    }
    // No "name" field: the env name is the FILENAME (EnvironmentStore sets it on load). Don't duplicate it.
    return json{{"schemaVersion", e.schemaVersion}, {"keys", keys}};
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
            ek.secret = getBool(k, "secret", false);
            e.keys.push_back(ek);
        }
    }
    return e;
}

// snake_case keys (app config). cacheResponses/cachePersist NOT exposed to user -> always default true.
json toJson(const AppConfig& c) {
    return json{{"last_collection_root", c.lastCollectionRoot},
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
} // namespace
AppConfig appConfigFromJson(const json& j) { return appConfigFromJson(j, AppConfig{}); }

AppConfig appConfigFromJson(const json& j, const AppConfig& def) {
    AppConfig c = def;   // missing key -> keep the default value (from .env)
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
    s.activeEnv = getStr(j, "activeEnv", "");   // empty = no env selected (no special base)
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
