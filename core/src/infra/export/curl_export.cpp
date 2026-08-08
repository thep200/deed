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

// Returns the index just past the substituted ":key", or npos when no enabled var matches at a path boundary.
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
    // Single left-to-right pass: one allocation, no repeated full-string find/replace.
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

std::string curlQueryString(const d::HttpRequest& h) {
    std::string qs;
    auto add = [&](const std::string& k, const std::string& v) {
        qs += (qs.empty() ? "?" : "&"); qs += k + "=" + v;
    };
    for (const auto& p : h.params().items())
        if (p.enabled() && !p.key().empty()) add(p.key(), p.value());
    return qs;
}

// Auth wins over a user-supplied Authorization header.
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
        else if constexpr (std::is_same_v<T, d::AuthOAuth2>)
            // Pure render (no token fetch) -> reference an env var; a mid-command shell comment would break the \-chain.
            cmd += " \\\n  -H \"Authorization: Bearer $OAUTH2_TOKEN\"";
    });
}

bool headersHaveContentType(const d::HeaderList& headers) {
    for (const auto& hd : headers.items())
        if (hd.enabled() && toLower(hd.name()) == "content-type") return true;
    return false;
}

// Adds a Content-Type only when none is present.
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

// One commandFor overload per payload type — dispatch by overload resolution, so a new type without a render is a compile error.
std::string commandFor(const d::HttpRequest& h, bool) {
    std::string url = applyPathVars(h.url().raw(), h.pathVariables()) + curlQueryString(h);
    std::string cmd = "curl -X " + d::toString(h.method()) + " " + shq(url);
    appendHeadersAndAuth(cmd, h.headers(), h.auth());
    appendBody(cmd, h);
    return cmd;
}

// tlsOn is the per-request effective TLS (config.tls); !tlsOn -> -plaintext.
std::string commandFor(const d::GrpcRequest& g, bool tlsOn) {
    std::string cmd = "grpcurl";
    if (!tlsOn) cmd += " -plaintext";
    const std::string& msg = g.message().text();
    if (!msg.empty() && msg != "{}") cmd += " \\\n  -d " + shq(msg);
    for (const auto& m : g.metadata().entries())
        if (m.enabled && !m.key.empty()) cmd += " \\\n  -H " + shq(m.key + ": " + m.value);
    cmd += " \\\n  " + g.target() + " " + g.service() + "/" + g.method();
    return cmd;
}

// Subscriptions actually run over WS/SSE, but export renders the universally runnable HTTP-POST form.
std::string commandFor(const d::GraphQlRequest& g, bool) {
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
std::string commandFor(const d::WebSocketRequest& w, bool) {
    std::string cmd = "curl " + shq(w.url().raw());
    appendHeadersAndAuth(cmd, w.headers(), w.auth());
    return cmd;
}

std::string commandFor(const d::KafkaRequest& k, bool) {
    const std::string brokers = k.brokers().toBootstrapServers();
    std::string cmd;
    k.match([&](auto&& spec) {
        using S = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<S, d::KafkaProduceSpec>) {
            const auto& c = spec.config;
            const auto& m = spec.message;
            cmd = "echo " + shq(m.value.value) + " | kcat -P -b " + shq(brokers) +
                  " -t " + shq(c.topic.value());
            if (c.partition.value >= 0) cmd += " -p " + std::to_string(c.partition.value);
            if (m.key) cmd += " -k " + shq(m.key->value);
            for (const auto& h : m.headers)
                if (h.enabled && !h.key.empty()) cmd += " \\\n  -H " + shq(h.key + "=" + h.value);
            if (c.valueFormat == d::KafkaValueFormat::Avro && !c.schemaRegistry.url.empty())
                cmd += " \\\n  -s value=avro -r " + shq(c.schemaRegistry.url);
        } else {
            const auto& c = spec.config;
            const char* reset =
                c.offsetReset == d::OffsetReset::Earliest ? "earliest" : "latest";
            if (!c.partition.has_value()) {
                cmd = "kcat -b " + shq(brokers) + " -G " + shq(c.group.value());
                for (const auto& t : c.topics) cmd += " " + shq(t.value());
            } else {
                cmd = "kcat -C -b " + shq(brokers);
                if (!c.topics.empty()) cmd += " -t " + shq(c.topics.front().value());
                cmd += " -p " + std::to_string(c.partition->value);
            }
            cmd += " \\\n  -X auto.offset.reset=" + std::string(reset);
            if (c.maxMessages) cmd += " -c " + std::to_string(*c.maxMessages);
        }
    });
    return cmd;
}

// Mirrors the sender's per-version Content-Type/SOAPAction policy.
std::string commandFor(const d::SoapRequest& s, bool) {
    std::string cmd = "curl -X POST " + shq(s.url().raw());
    appendHeadersAndAuth(cmd, s.headers(), s.auth());
    if (!headersHaveContentType(s.headers())) {
        std::string ct = s.version() == d::SoapVersion::V1_2
                             ? (s.action().empty()
                                    ? std::string("application/soap+xml; charset=utf-8")
                                    : "application/soap+xml; charset=utf-8; action=\"" + s.action() + "\"")
                             : std::string("text/xml; charset=utf-8");
        cmd += " \\\n  -H " + shq("Content-Type: " + ct);
    }
    if (s.version() == d::SoapVersion::V1_1)
        cmd += " \\\n  -H " + shq("SOAPAction: \"" + s.action() + "\"");
    cmd += " \\\n  --data " + shq(s.envelope());
    return cmd;
}

// -x = simple bind (like the sender); the bind-test second bind has no ldapsearch equivalent — search only.
std::string commandFor(const d::LdapRequest& l, bool) {
    std::string cmd = "ldapsearch -x -H " + shq(l.url().raw());
    if (l.startTls()) cmd += " -ZZ";   // require a successful StartTLS, not best-effort
    if (!l.bindDn().empty()) {
        cmd += " \\\n  -D " + shq(l.bindDn());
        if (!l.bindPassword().empty()) cmd += " -w " + shq(l.bindPassword());
    }
    cmd += " \\\n  -b " + shq(l.baseDn()) + " -s " + d::toString(l.scope());
    if (l.sizeLimit() > 0) cmd += " -z " + std::to_string(l.sizeLimit());
    if (l.timeLimit() > 0) cmd += " -l " + std::to_string(l.timeLimit());
    if (l.pageSize() > 0) cmd += " -E " + shq("pr=" + std::to_string(l.pageSize()) + "/noprompt");
    cmd += " \\\n  " + shq(d::composeLdapFilter(l.filter(), l.group()));
    for (const auto& a : l.attributes()) if (!a.empty()) cmd += " " + shq(a);
    return cmd;
}

} // namespace

std::string toCurl(const d::RequestModel& m) {
    const bool tlsOn = m.config().tlsEnabledDefault;
    return m.match([&](const auto& p) { return commandFor(p, tlsOn); });
}

} // namespace core
