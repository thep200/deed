#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/dto_common.hpp" // surviving KeyValue / MultipartPart DTOs (parse scratch only)
#include "core/import_export/importer.hpp"
#include "import_export/shell_tokenize.hpp"
#include "infra/url_util.hpp"

namespace core {

namespace d = core::domain;

namespace {

// ---- mutable parse scratch (decoupled from any persisted type) ------------------------------------------
// The per-token handlers fill this; buildHttpDomain() then converts it to an immutable domain RequestModel.
// Reuses the surviving KeyValue / MultipartPart value DTOs (dto_common.hpp). It is NOT the persisted model.
struct SBody {
    std::string mode = "none"; // none|json|text|xml|form-urlencoded|multipart|binary
    std::string json, text, xml;
    std::vector<KeyValue> formUrlEncoded;
    std::vector<MultipartPart> multipart;
    std::string binaryFilePath;
};
struct SAuth {
    std::string type = "none"; // none|basic|bearer|apikey
    std::string bearerToken, basicUsername, basicPassword;
    std::string apikeyKey, apikeyValue, apikeyIn = "header";
};
struct SHttp {
    std::string method, url;
    std::vector<KeyValue> headers, params;
    SBody body;
    SAuth auth;
    // imported intent carried into the per-request Config:
    int timeoutMs = 0; bool timeoutMsSet = false;
    bool verifyTls = true; bool verifyTlsSet = false;
};
// unknown-flag / error accumulator threaded through the handlers.
struct Acc {
    std::vector<std::string> unknown;
    std::string error;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Split "key: value" from -H.
KeyValue parseHeader(const std::string& raw) {
    KeyValue kv;
    kv.enabled = true;
    size_t colon = raw.find(':');
    if (colon == std::string::npos) { kv.key = trim(raw); return kv; }
    kv.key = trim(raw.substr(0, colon));
    kv.value = trim(raw.substr(colon + 1));
    return kv;
}

bool isJsonLike(const std::string& s) {
    std::string t = trim(s);
    return !t.empty() && (t.front() == '{' || t.front() == '[');
}

// Well-formed base64? (M15) Non-empty, length multiple of 4, only alphabet/'=' (padding only at the end).
// Guards against treating a garbled Basic token as valid credentials.
bool looksBase64(const std::string& s) {
    if (s.empty() || s.size() % 4 != 0) return false;
    bool padding = false;
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/';
        if (c == '=') { padding = true; continue; }
        if (padding || !ok) return false;   // data char after padding, or stray char -> invalid
    }
    return true;
}

// Decode base64 (skip unknown chars/whitespace; stop at '='). Used to split Basic user:pass.
std::string base64Decode(const std::string& in) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(unsigned char)chars[i]] = i;
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

// Route the Authorization header value into the pane's Auth tab (instead of the headers list).
// Bearer -> bearer token; Basic -> base64-decode to user:pass; other scheme -> apikey header.
void applyAuthHeader(SAuth& auth, const std::string& rawValue) {
    std::string v = trim(rawValue);
    size_t sp = v.find(' ');
    std::string scheme = (sp == std::string::npos) ? "" : lower(v.substr(0, sp));
    std::string rest = (sp == std::string::npos) ? "" : trim(v.substr(sp + 1));
    if (scheme == "bearer") {
        auth.type = "bearer";
        auth.bearerToken = rest;
    } else if (scheme == "basic" && looksBase64(rest)) {
        std::string decoded = base64Decode(rest);
        size_t c = decoded.find(':');
        auth.type = "basic";
        auth.basicUsername = (c == std::string::npos) ? decoded : decoded.substr(0, c);
        auth.basicPassword = (c == std::string::npos) ? "" : decoded.substr(c + 1);
    } else {
        auth.type = "apikey";          // unknown/missing scheme -> keep as-is in Auth tab
        auth.apikeyKey = "Authorization";
        auth.apikeyValue = v;
        auth.apikeyIn = "header";
    }
}

void pushHeader(SHttp& h, const char* key, const std::string& value) {
    KeyValue kv; kv.enabled = true; kv.key = key; kv.value = value;
    h.headers.push_back(kv);
}

// One -F/--form field: "key=value" (value "@path" -> file part, else text part).
void addMultipart(SHttp& h, const std::string& f) {
    size_t eq = f.find('=');
    MultipartPart p;
    p.key = eq == std::string::npos ? f : f.substr(0, eq);
    std::string v = eq == std::string::npos ? "" : f.substr(eq + 1);
    if (!v.empty() && v.front() == '@') { p.type = "file"; p.filePath = v.substr(1); }
    else { p.type = "text"; p.value = v; }
    h.body.multipart.push_back(p);
    h.body.mode = "multipart";
}

// Split "a=b&c=d" into KeyValue pairs (value empty when '=' is missing). Empty input -> empty vector.
std::vector<KeyValue> splitKvPairs(const std::string& joined) {
    std::vector<KeyValue> out;
    size_t pos = 0;
    while (pos < joined.size()) {
        size_t amp = joined.find('&', pos);
        std::string seg = joined.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = seg.find('=');
        KeyValue kv;
        kv.key = eq == std::string::npos ? seg : seg.substr(0, eq);
        kv.value = eq == std::string::npos ? "" : seg.substr(eq + 1);
        out.push_back(kv);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

// Mutable accumulators threaded through the per-token handlers.
struct CurlParseState {
    SHttp& h;
    Acc& acc;
    std::vector<std::string> dataParts;   // -d / --data
    std::vector<KeyValue> urlEncodeParts; // --data-urlencode
    bool getStyle = false;                // -G
    bool explicitMethod = false;
    std::string contentType;
};

// curl flags that consume the FOLLOWING token as their value (unless given inline as --flag=value).
bool curlFlagTakesValue(const std::string& flag) {
    return flag == "-X" || flag == "--request" || flag == "-H" || flag == "--header" ||
           flag == "-u" || flag == "--user" || flag == "--data-binary" ||
           flag == "-d" || flag == "--data" || flag == "--data-raw" || flag == "--data-ascii" ||
           flag == "-A" || flag == "--user-agent" || flag == "-e" || flag == "--referer" ||
           flag == "-b" || flag == "--cookie" || flag == "-m" || flag == "--max-time" ||
           flag == "--connect-timeout" || flag == "--data-urlencode" || flag == "-F" || flag == "--form" ||
           flag == "--url" || flag == "-o" || flag == "--output";
}

// output/log flags with no effect on the request model (any value is already consumed before dispatch).
bool isIgnoredCurlFlag(const std::string& flag) {
    return flag == "-s" || flag == "--silent" || flag == "-i" || flag == "--include" ||
           flag == "-v" || flag == "--verbose" || flag == "-#" || flag == "--progress-bar" ||
           flag == "-o" || flag == "--output" || flag == "-O" || flag == "--remote-name";
}

// Each handler returns true if it recognized `flag`. Grouped to keep every chain small.
bool curlAuthMethodFlag(const std::string& flag, const std::string& value, CurlParseState& st) {
    SHttp& h = st.h;
    if (flag == "-X" || flag == "--request") { h.method = value; st.explicitMethod = true; }
    else if (flag == "-I" || flag == "--head") { h.method = "HEAD"; st.explicitMethod = true; }
    else if (flag == "-u" || flag == "--user") {
        size_t c = value.find(':');
        h.auth.type = "basic";
        h.auth.basicUsername = c == std::string::npos ? value : value.substr(0, c);
        h.auth.basicPassword = c == std::string::npos ? "" : value.substr(c + 1);
    } else return false;
    return true;
}

bool curlHeaderFlag(const std::string& flag, const std::string& value, CurlParseState& st) {
    SHttp& h = st.h;
    if (flag == "-H" || flag == "--header") {
        KeyValue kv = parseHeader(value);
        std::string lk = lower(kv.key);
        if (lk == "content-type") st.contentType = lower(kv.value);
        if (lk == "authorization") applyAuthHeader(h.auth, kv.value);   // -> Auth tab (not the headers list)
        else h.headers.push_back(kv);
    } else if (flag == "-A" || flag == "--user-agent") pushHeader(h, "User-Agent", value);
    else if (flag == "-e" || flag == "--referer") pushHeader(h, "Referer", value);
    else if (flag == "-b" || flag == "--cookie") pushHeader(h, "Cookie", value);
    else if (flag == "--compressed") pushHeader(h, "Accept-Encoding", "gzip, deflate");
    else return false;
    return true;
}

bool curlDataFlag(const std::string& flag, const std::string& value, CurlParseState& st) {
    SHttp& h = st.h;
    if (flag == "--data-binary") {
        if (!value.empty() && value.front() == '@') { h.body.mode = "binary"; h.body.binaryFilePath = value.substr(1); }
        else st.dataParts.push_back(value);
    } else if (flag == "-d" || flag == "--data" || flag == "--data-raw" || flag == "--data-ascii") {
        st.dataParts.push_back(value);
    } else if (flag == "--data-urlencode") {
        size_t eq = value.find('=');
        KeyValue kv;
        if (eq == std::string::npos) kv.value = value;
        else { kv.key = value.substr(0, eq); kv.value = value.substr(eq + 1); }
        st.urlEncodeParts.push_back(kv);
    } else if (flag == "-F" || flag == "--form") {
        addMultipart(h, value);
    } else return false;
    return true;
}

bool curlOptionFlag(const std::string& flag, const std::string& value, CurlParseState& st) {
    SHttp& h = st.h;
    if (flag == "-G" || flag == "--get") st.getStyle = true;
    else if (flag == "-L" || flag == "--location") { /* follow-redirects: no domain field, recognized no-op */ }
    else if (flag == "-k" || flag == "--insecure") { h.verifyTls = false; h.verifyTlsSet = true; }
    else if (flag == "--url") h.url = value;
    else if (flag == "-m" || flag == "--max-time" || flag == "--connect-timeout") {
        try { h.timeoutMs = (int)(std::stod(value) * 1000); h.timeoutMsSet = true; } catch (...) {}
    } else if (isIgnoredCurlFlag(flag)) {
        // no effect on the request model
    } else return false;
    return true;
}

// Process one token: split an inline --flag=value, fetch a value for value-flags, then dispatch.
void applyCurlToken(const std::vector<std::string>& tokens, size_t& i, CurlParseState& st) {
    const std::string& tk = tokens[i];
    std::string flag = tk;
    std::string value;
    bool hasInline = false; // M14: track presence, not emptiness (so `--data=` -> empty value, not next token)
    if (tk.rfind("--", 0) == 0) {
        size_t eq = tk.find('=');
        if (eq != std::string::npos) { flag = tk.substr(0, eq); value = tk.substr(eq + 1); hasInline = true; }
    }
    if (!hasInline && curlFlagTakesValue(flag))
        value = (i + 1 < tokens.size()) ? tokens[++i] : "";

    if (curlAuthMethodFlag(flag, value, st)) return;
    if (curlHeaderFlag(flag, value, st)) return;
    if (curlDataFlag(flag, value, st)) return;
    if (curlOptionFlag(flag, value, st)) return;

    if (!tk.empty() && tk[0] == '-') st.acc.unknown.push_back(tk); // unknown flag
    else if (st.h.url.empty()) st.h.url = tk;                      // first bare token = URL
    else st.acc.unknown.push_back(tk);
}

// Merge collected -d/--data (and --data-urlencode) into the request body or query (for -G).
void applyCurlData(CurlParseState& st) {
    SHttp& h = st.h;
    if (!st.dataParts.empty()) {
        std::string joined;
        for (size_t i = 0; i < st.dataParts.size(); ++i) { if (i) joined += "&"; joined += st.dataParts[i]; }
        if (st.getStyle) {
            for (auto& kv : splitKvPairs(joined)) h.params.push_back(kv);     // -G: data -> query params
        } else if (st.contentType.find("application/x-www-form-urlencoded") != std::string::npos) {
            h.body.mode = "form-urlencoded";
            for (auto& kv : splitKvPairs(joined)) h.body.formUrlEncoded.push_back(kv);
        } else if (st.contentType.find("application/json") != std::string::npos || isJsonLike(joined)) {
            h.body.mode = "json"; h.body.json = joined;
        } else {
            h.body.mode = "text"; h.body.text = joined;
        }
    }
    if (!st.urlEncodeParts.empty() && h.body.mode != "form-urlencoded") {
        h.body.mode = "form-urlencoded";
        for (auto& kv : st.urlEncodeParts) h.body.formUrlEncoded.push_back(kv);
    }
}

// ---- scratch -> immutable domain RequestModel (no legacy RequestModel, no request_bridge) ----------------
d::Auth authToDomain(const SAuth& a) {
    if (a.type == "bearer") { auto r = d::Auth::bearer(a.bearerToken); return r ? r.take() : d::Auth::none(); }
    if (a.type == "basic") {
        auto r = d::Auth::basic(a.basicUsername, a.basicPassword);
        return r ? r.take() : d::Auth::none();
    }
    if (a.type == "apikey") {
        auto r = d::Auth::apiKey(a.apikeyKey, a.apikeyValue,
                                 a.apikeyIn == "query" ? d::ApiKeyIn::Query : d::ApiKeyIn::Header);
        return r ? r.take() : d::Auth::none();
    }
    return d::Auth::none();
}

d::Body bodyToDomain(const SBody& b) {
    if (b.mode == "json") return d::Body::raw(d::RawSubtype::Json, b.json);
    if (b.mode == "text") return d::Body::raw(d::RawSubtype::Text, b.text);
    if (b.mode == "xml") return d::Body::raw(d::RawSubtype::Xml, b.xml);
    if (b.mode == "form-urlencoded") {
        std::vector<d::FormField> f;
        for (const auto& kv : b.formUrlEncoded) f.push_back({kv.key, kv.value, kv.enabled});
        return d::Body::formUrlEncoded(std::move(f));
    }
    if (b.mode == "multipart") {
        std::vector<d::MultipartPart> mp;
        for (const auto& p : b.multipart)
            mp.push_back({p.key, p.type == "file" ? d::PartKind::File : d::PartKind::Text, p.value,
                          p.filePath, p.enabled});
        auto r = d::Body::multipart(std::move(mp));
        return r ? r.take() : d::Body::none();
    }
    if (b.mode == "binary") { auto r = d::Body::binary(b.binaryFilePath); return r ? r.take() : d::Body::none(); }
    return d::Body::none();
}

d::RequestModel buildHttpDomain(const SHttp& h, const std::string& name) {
    std::vector<d::Header> hdrs;
    for (const auto& kv : h.headers) {
        auto r = d::Header::create(kv.key, kv.value, kv.enabled);
        if (r) hdrs.push_back(r.take());
    }
    std::vector<d::QueryParam> prms;
    for (const auto& kv : h.params) {
        auto r = d::QueryParam::create(kv.key, kv.value, kv.enabled);
        if (r) prms.push_back(r.take());
    }
    auto methodR = d::parseHttpMethod(h.method);
    d::HttpRequest::Parts hp{methodR ? methodR.take() : d::HttpMethod::Get,
                             d::Url::create(h.url).take(),
                             d::PathVariableList{}, // cURL has no `:path` variables
                             d::QueryParamList(std::move(prms)),
                             d::HeaderList(std::move(hdrs)),
                             bodyToDomain(h.body),
                             authToDomain(h.auth)};
    auto http = d::HttpRequest::create(std::move(hp)).take();
    int timeoutMs = (h.timeoutMsSet && h.timeoutMs > 0) ? h.timeoutMs : 1800000;
    bool tls = h.verifyTlsSet ? h.verifyTls : true;
    d::RequestConfig cfg{d::Timeout::fromMillis(timeoutMs).take(), tls};
    return d::RequestModel::create(d::RequestId(""), name, 0, cfg, http).take();
}

} // namespace

bool CurlImporter::canHandle(const std::string& input) const {
    std::string t = lower(trim(input.substr(0, 16)));   // only need the prefix; don't copy a huge paste (L8)
    return t.rfind("curl", 0) == 0;
}

ImportParseResult CurlImporter::parse(const std::string& input) const {
    Acc acc; // local accumulator for unknown/error
    auto tokens = shellTokenize(input);
    if (tokens.empty() || lower(tokens[0]) != "curl")
        return {false, std::nullopt, {}, "not a cURL command"};

    SHttp h;
    CurlParseState st{h, acc};
    for (size_t i = 1; i < tokens.size(); ++i) applyCurlToken(tokens, i, st);
    applyCurlData(st);

    // Infer method if not declared: body -> POST, otherwise GET.
    if (!st.explicitMethod) h.method = (h.body.mode != "none") ? "POST" : "GET";

    if (h.url.empty())
        return {false, std::nullopt, {}, "no URL found in cURL command"};

    // Query after '?' -> split into params (decoded), remaining url is raw.
    urlutil::splitUrlQuery(h.url, h.params);

    return {true, buildHttpDomain(h, "Imported cURL"), acc.unknown, ""};
}

} // namespace core
