#include <cctype>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "core/domain/request/request_model.hpp"
#include "core/infra/export/exporter.hpp"

namespace core {
namespace d = core::domain;

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
                         const d::PathVariableList& vars, std::string& out) {
    if (url[i] != ':') return std::string::npos;
    for (const auto& v : vars.items()) {
        if (!v.enabled() || v.key().empty()) continue;
        if (url.compare(i + 1, v.key().size(), v.key()) != 0) continue;
        std::size_t end = i + 1 + v.key().size();
        char nxt = end < url.size() ? url[end] : '/';
        if (nxt == '/' || nxt == '?' || nxt == '&' || end == url.size()) {
            out += v.value();
            return end;
        }
    }
    return std::string::npos;
}

std::string applyPathVars(const std::string& url, const d::PathVariableList& vars) {
    if (vars.items().empty()) return url;
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

std::string toLower(std::string s) { for (auto& c : s) c = (char)::tolower((unsigned char)c); return s; }

// Build the "?a=b&c=d" query string from enabled params (+ apikey-in-query). Empty if no params.
std::string curlQueryString(const d::HttpRequest& h) {
    std::string qs;
    auto add = [&](const std::string& k, const std::string& v) {
        qs += (qs.empty() ? "?" : "&"); qs += k + "=" + v;
    };
    for (const auto& p : h.params().items())
        if (p.enabled() && !p.key().empty()) add(p.key(), p.value());
    h.auth().match([&](auto&& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, d::AuthApiKey>)
            if (a.in == d::ApiKeyIn::Query) add(a.key, a.value);
    });
    return qs;
}

// Append enabled `-H` headers and the auth flag (bearer/basic/apikey). Auth wins over an Authorization header.
void appendHeadersAndAuth(std::string& cmd, const d::HeaderList& headers, const d::Auth& auth) {
    bool authActive = !auth.isNone();
    for (const auto& hd : headers.items()) {
        if (!hd.enabled() || hd.name().empty()) continue;
        if (authActive && toLower(hd.name()) == "authorization") continue; // auth wins
        cmd += " \\\n  -H " + shq(hd.name() + ": " + hd.value());
    }
    auth.match([&](auto&& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, d::AuthBearer>)
            cmd += " \\\n  -H " + shq("Authorization: Bearer " + a.token);
        else if constexpr (std::is_same_v<T, d::AuthBasic>)
            cmd += " \\\n  -u " + shq(a.username + ":" + a.password);
        else if constexpr (std::is_same_v<T, d::AuthApiKey>)
            if (a.in == d::ApiKeyIn::Header) cmd += " \\\n  -H " + shq(a.key + ": " + a.value);
    });
}

bool headersHaveContentType(const d::HeaderList& headers) {
    for (const auto& hd : headers.items())
        if (hd.enabled() && toLower(hd.name()) == "content-type") return true;
    return false;
}

// Append the body flags for the body alternative (adds a Content-Type unless one is already present).
void appendBody(std::string& cmd, const d::HttpRequest& h) {
    bool hasContentType = headersHaveContentType(h.headers());
    auto ct = [&](const char* v) {
        return hasContentType ? std::string() : " \\\n  -H " + shq(std::string("Content-Type: ") + v);
    };
    h.body().match([&](auto&& b) {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, d::BodyRaw>) {
            if (b.subtype == d::RawSubtype::Json) cmd += ct("application/json") + " \\\n  --data " + shq(b.text);
            else if (b.subtype == d::RawSubtype::Xml) cmd += ct("application/xml") + " \\\n  --data " + shq(b.text);
            else cmd += " \\\n  --data " + shq(b.text); // Text: no implicit Content-Type
        } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
            for (const auto& f : b.fields)
                if (f.enabled) cmd += " \\\n  --data-urlencode " + shq(f.key + "=" + f.value);
        } else if constexpr (std::is_same_v<T, d::BodyMultipart>) {
            for (const auto& p : b.parts)
                if (p.enabled)
                    cmd += " \\\n  -F " +
                           shq(p.key + "=" + (p.kind == d::PartKind::File ? "@" + p.filePath : p.value));
        } else if constexpr (std::is_same_v<T, d::BodyBinary>) {
            cmd += " \\\n  --data-binary " + shq("@" + b.filePath);
        }
        // BodyNone: no body flags.
    });
}

std::string curlHttp(const d::HttpRequest& h) {
    std::string url = applyPathVars(h.url().raw(), h.pathVariables()) + curlQueryString(h);
    std::string cmd = "curl -X " + d::toString(h.method()) + " " + shq(url);
    appendHeadersAndAuth(cmd, h.headers(), h.auth());
    appendBody(cmd, h);
    return cmd;
}

// gRPC: `tlsOn` is the per-request effective TLS (config.tls); !tlsOn -> -plaintext (matches the legacy
// applyRequestConfig that copied config.tls onto grpc.tls.enabled before exporting).
std::string curlGrpc(const d::GrpcRequest& g, bool tlsOn) {
    std::string cmd = "grpcurl";
    if (!tlsOn) cmd += " -plaintext";
    const std::string& msg = g.message().text();
    if (!msg.empty() && msg != "{}") cmd += " \\\n  -d " + shq(msg);
    for (const auto& m : g.metadata().entries())
        if (m.enabled && !m.key.empty()) cmd += " \\\n  -H " + shq(m.key + ": " + m.value);
    cmd += " \\\n  " + g.target() + " " + g.service() + "/" + g.method();
    return cmd;
}

// GraphQL over HTTP: a POST carrying {query, operationName?, variables}. (Subscriptions actually run over
// WS/SSE, but "copy as cURL" renders the HTTP-POST form — the universally runnable shape.)
std::string curlGraphQl(const d::GraphQlRequest& g) {
    nlohmann::json body;
    body["query"] = g.op().query;
    if (!g.op().operationName.empty()) body["operationName"] = g.op().operationName;
    try { body["variables"] = nlohmann::json::parse(g.op().variables.text()); }
    catch (...) { body["variables"] = nlohmann::json::object(); }
    std::string cmd = "curl -X POST " + shq(g.url().raw());
    appendHeadersAndAuth(cmd, g.headers(), g.auth());
    if (!headersHaveContentType(g.headers())) cmd += " \\\n  -H " + shq("Content-Type: application/json");
    cmd += " \\\n  --data " + shq(body.dump());
    return cmd;
}

// WebSocket has no faithful one-shot cURL; emit the handshake URL + headers/auth as a best-effort reference.
std::string curlWs(const d::WebSocketRequest& w) {
    std::string cmd = "curl " + shq(w.url().raw());
    appendHeadersAndAuth(cmd, w.headers(), w.auth());
    return cmd;
}

} // namespace

std::string toCurl(const d::RequestModel& m) {
    const bool tlsOn = m.config().tlsEnabledDefault;
    std::string out;
    m.match([&](auto&& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, d::HttpRequest>) out = curlHttp(p);
        else if constexpr (std::is_same_v<T, d::GrpcRequest>) out = curlGrpc(p, tlsOn);
        else if constexpr (std::is_same_v<T, d::GraphQlRequest>) out = curlGraphQl(p);
        else if constexpr (std::is_same_v<T, d::WebSocketRequest>) out = curlWs(p);
    });
    return out;
}

} // namespace core
