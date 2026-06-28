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

// Try to substitute a ":key" path variable at url[i]. On a match, append its value to `out` and return
// the index just past the key. Returns npos when no enabled path-var matches at a path boundary.
std::size_t matchPathVar(const std::string& url, std::size_t i,
                         const std::vector<KeyValue>& vars, std::string& out) {
    if (url[i] != ':') return std::string::npos;
    for (const auto& v : vars) {
        if (!v.enabled || v.key.empty()) continue;
        if (url.compare(i + 1, v.key.size(), v.key) != 0) continue;
        std::size_t end = i + 1 + v.key.size();
        char nxt = end < url.size() ? url[end] : '/';
        if (nxt == '/' || nxt == '?' || nxt == '&' || end == url.size()) {
            out += v.value;
            return end;
        }
    }
    return std::string::npos;
}

std::string applyPathVars(const std::string& url, const std::vector<KeyValue>& vars) {
    if (vars.empty()) return url;
    // Single left-to-right pass (M18): at each ':' try to match a path-var key (with a path boundary after)
    // and substitute. One allocation, no repeated full-string find/replace + realloc.
    std::string out;
    out.reserve(url.size() + 16);
    for (std::size_t i = 0; i < url.size();) {
        std::size_t next = matchPathVar(url, i, vars, out);
        if (next != std::string::npos) { i = next; continue; }
        out += url[i++];
    }
    return out;
}

std::string toLower(std::string s) { for (auto& c : s) c = (char)::tolower(c); return s; }

// Build the "?a=b&c=d" query string from enabled params (+ apikey-in-query). Empty if no params.
std::string curlQueryString(const HttpRequest& h) {
    std::string qs;
    auto add = [&](const std::string& k, const std::string& v) {
        qs += (qs.empty() ? "?" : "&"); qs += k + "=" + v;
    };
    for (const auto& p : h.params)
        if (p.enabled && !p.key.empty()) add(p.key, p.value);
    if (h.auth.type == "apikey" && h.auth.apikeyIn == "query")
        add(h.auth.apikeyKey, h.auth.apikeyValue);
    return qs;
}

// Append enabled `-H` headers and the auth flag (bearer/basic/apikey). Auth wins over an Authorization header.
void appendCurlHeadersAndAuth(std::string& cmd, const HttpRequest& h) {
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
}

bool headersHaveContentType(const HttpRequest& h) {
    for (const auto& hd : h.headers)
        if (hd.enabled && toLower(hd.key) == "content-type") return true;
    return false;
}

void appendFormBody(std::string& cmd, const Body& b) {
    for (const auto& kv : b.formUrlEncoded)
        if (kv.enabled) cmd += " \\\n  --data-urlencode " + shq(kv.key + "=" + kv.value);
}

void appendMultipartBody(std::string& cmd, const Body& b) {
    for (const auto& p : b.multipart)
        if (p.enabled) cmd += " \\\n  -F " + shq(p.key + "=" + (p.type == "file" ? "@" + p.filePath : p.value));
}

// Append the body flags for the current body mode (adds a Content-Type unless one is already present).
void appendCurlBody(std::string& cmd, const HttpRequest& h) {
    bool hasContentType = headersHaveContentType(h);
    auto ct = [&](const char* v) {
        return hasContentType ? std::string() : " \\\n  -H " + shq(std::string("Content-Type: ") + v);
    };

    const Body& b = h.body;
    if (b.mode == "json") cmd += ct("application/json") + " \\\n  --data " + shq(b.json);
    else if (b.mode == "text") cmd += " \\\n  --data " + shq(b.text);
    else if (b.mode == "xml") cmd += ct("application/xml") + " \\\n  --data " + shq(b.xml);
    else if (b.mode == "graphql") cmd += ct("application/json") + " \\\n  --data " + shq(b.graphqlQuery);
    else if (b.mode == "form-urlencoded") appendFormBody(cmd, b);
    else if (b.mode == "multipart") appendMultipartBody(cmd, b);
    else if (b.mode == "binary") cmd += " \\\n  --data-binary " + shq("@" + b.binaryFilePath);
}

std::string curlHttp(const HttpRequest& h) {
    std::string url = applyPathVars(h.url, h.pathVariables) + curlQueryString(h);
    std::string cmd = "curl -X " + (h.method.empty() ? std::string("GET") : h.method) + " " + shq(url);
    appendCurlHeadersAndAuth(cmd, h);
    appendCurlBody(cmd, h);
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
