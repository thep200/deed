#include <string>

#include "core/import_export/importer.hpp"

namespace core {

namespace {

// Shell-safe single-quote wrap: ' -> '\''
std::string shq(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string applyPathVars(std::string url, const std::vector<KeyValue>& vars) {
    for (const auto& v : vars) {
        if (!v.enabled || v.key.empty()) continue;
        std::string token = ":" + v.key;
        size_t pos = 0;
        while ((pos = url.find(token, pos)) != std::string::npos) {
            size_t end = pos + token.size();
            char nxt = end < url.size() ? url[end] : '/';
            if (nxt == '/' || nxt == '?' || nxt == '&' || end == url.size()) {
                url.replace(pos, token.size(), v.value);
                pos += v.value.size();
            } else pos = end;
        }
    }
    return url;
}

std::string toLower(std::string s) { for (auto& c : s) c = (char)::tolower(c); return s; }

std::string curlHttp(const HttpRequest& h) {
    std::string url = applyPathVars(h.url, h.pathVariables);
    // query params
    std::string qs;
    for (const auto& p : h.params)
        if (p.enabled && !p.key.empty()) { qs += (qs.empty() ? "?" : "&"); qs += p.key + "=" + p.value; }
    // apikey in query
    if (h.auth.type == "apikey" && h.auth.apikeyIn == "query")
        { qs += (qs.empty() ? "?" : "&"); qs += h.auth.apikeyKey + "=" + h.auth.apikeyValue; }
    url += qs;

    std::string cmd = "curl -X " + (h.method.empty() ? std::string("GET") : h.method) + " " + shq(url);

    bool authActive = (h.auth.type != "none" && !h.auth.type.empty());
    for (const auto& hd : h.headers) {
        if (!hd.enabled || hd.key.empty()) continue;
        if (authActive && toLower(hd.key) == "authorization") continue; // auth wins
        cmd += " \\\n  -H " + shq(hd.key + ": " + hd.value);
    }
    if (h.auth.type == "bearer") cmd += " \\\n  -H " + shq("Authorization: Bearer " + h.auth.bearerToken);
    else if (h.auth.type == "basic") cmd += " \\\n  -u " + shq(h.auth.basicUsername + ":" + h.auth.basicPassword);
    else if (h.auth.type == "apikey" && h.auth.apikeyIn == "header")
        cmd += " \\\n  -H " + shq(h.auth.apikeyKey + ": " + h.auth.apikeyValue);

    // Content-Type already in headers? (avoid duplicate)
    bool hasContentType = false;
    for (const auto& hd : h.headers)
        if (hd.enabled && toLower(hd.key) == "content-type") hasContentType = true;
    auto ct = [&](const char* v) { return hasContentType ? std::string() : " \\\n  -H " + shq(std::string("Content-Type: ") + v); };

    const Body& b = h.body;
    if (b.mode == "json") cmd += ct("application/json") + " \\\n  --data " + shq(b.json);
    else if (b.mode == "text") cmd += " \\\n  --data " + shq(b.text);
    else if (b.mode == "xml") cmd += ct("application/xml") + " \\\n  --data " + shq(b.xml);
    else if (b.mode == "graphql") cmd += ct("application/json") + " \\\n  --data " + shq(b.graphqlQuery);
    else if (b.mode == "form-urlencoded")
        for (const auto& kv : b.formUrlEncoded) if (kv.enabled) cmd += " \\\n  --data-urlencode " + shq(kv.key + "=" + kv.value);
    else if (b.mode == "multipart")
        for (const auto& p : b.multipart) if (p.enabled)
            cmd += " \\\n  -F " + shq(p.key + "=" + (p.type == "file" ? "@" + p.filePath : p.value));
    else if (b.mode == "binary") cmd += " \\\n  --data-binary " + shq("@" + b.binaryFilePath);

    return cmd;
}

std::string curlGrpc(const GrpcRequest& g) {
    std::string cmd = "grpcurl";
    if (!g.tls.enabled) cmd += " -plaintext";
    if (!g.message.empty() && g.message != "{}") cmd += " \\\n  -d " + shq(g.message);
    for (const auto& m : g.metadata)
        if (m.enabled && !m.key.empty()) cmd += " \\\n  -H " + shq(m.key + ": " + m.value);
    cmd += " \\\n  " + g.target + " " + g.service + "/" + g.method;
    return cmd;
}

} // namespace

std::string toCurl(const RequestModel& m) {
    return m.type == RequestType::Grpc ? curlGrpc(m.grpc) : curlHttp(m.http);
}

} // namespace core
