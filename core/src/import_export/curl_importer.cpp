#include <algorithm>
#include <cctype>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/import_export/importer.hpp"
#include "import_export/shell_tokenize.hpp"
#include "infra/url_util.hpp"

namespace core {

namespace {

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
void applyAuthHeader(HttpRequest& h, const std::string& rawValue) {
    std::string v = trim(rawValue);
    size_t sp = v.find(' ');
    std::string scheme = (sp == std::string::npos) ? "" : lower(v.substr(0, sp));
    std::string rest = (sp == std::string::npos) ? "" : trim(v.substr(sp + 1));
    if (scheme == "bearer") {
        h.auth.type = "bearer";
        h.auth.bearerToken = rest;
    } else if (scheme == "basic") {
        std::string decoded = base64Decode(rest);
        size_t c = decoded.find(':');
        h.auth.type = "basic";
        h.auth.basicUsername = (c == std::string::npos) ? decoded : decoded.substr(0, c);
        h.auth.basicPassword = (c == std::string::npos) ? "" : decoded.substr(c + 1);
    } else {
        h.auth.type = "apikey";          // unknown/missing scheme -> keep as-is in Auth tab
        h.auth.apikeyKey = "Authorization";
        h.auth.apikeyValue = v;
        h.auth.apikeyIn = "header";
    }
}

} // namespace

bool CurlImporter::canHandle(const std::string& input) const {
    std::string t = lower(trim(input));
    return t.rfind("curl", 0) == 0;
}

ImportResult CurlImporter::parse(const std::string& input) const {
    ImportResult res;
    auto tokens = shellTokenize(input);
    if (tokens.empty() || lower(tokens[0]) != "curl") {
        res.error = "not a cURL command";
        return res;
    }

    RequestModel m;
    m.type = RequestType::Http;
    m.name = "Imported cURL";
    HttpRequest& h = m.http;

    std::vector<std::string> dataParts;     // -d / --data
    std::vector<KeyValue> urlEncodeParts;    // --data-urlencode
    bool getStyle = false;                   // -G
    bool explicitMethod = false;
    std::string contentType;

    auto nextArg = [&](size_t& i) -> std::string {
        if (i + 1 < tokens.size()) return tokens[++i];
        return "";
    };

    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& tk = tokens[i];
        std::string flag = tk;
        std::string inlineVal;
        // support --flag=value
        if (tk.rfind("--", 0) == 0) {
            size_t eq = tk.find('=');
            if (eq != std::string::npos) { flag = tk.substr(0, eq); inlineVal = tk.substr(eq + 1); }
        }
        auto val = [&](size_t& idx) { return !inlineVal.empty() ? inlineVal : nextArg(idx); };

        if (flag == "-X" || flag == "--request") {
            h.method = val(i); explicitMethod = true;
        } else if (flag == "-H" || flag == "--header") {
            KeyValue kv = parseHeader(val(i));
            std::string lk = lower(kv.key);
            if (lk == "content-type") contentType = lower(kv.value);
            if (lk == "authorization") { applyAuthHeader(h, kv.value); continue; }  // -> Auth tab
            h.headers.push_back(kv);
        } else if (flag == "-u" || flag == "--user") {
            std::string up = val(i);
            size_t c = up.find(':');
            h.auth.type = "basic";
            h.auth.basicUsername = c == std::string::npos ? up : up.substr(0, c);
            h.auth.basicPassword = c == std::string::npos ? "" : up.substr(c + 1);
        } else if (flag == "--data-binary") {
            std::string v = val(i);
            if (!v.empty() && v.front() == '@') { h.body.mode = "binary"; h.body.binaryFilePath = v.substr(1); }
            else dataParts.push_back(v);
        } else if (flag == "-d" || flag == "--data" || flag == "--data-raw" || flag == "--data-ascii") {
            dataParts.push_back(val(i));
        } else if (flag == "-A" || flag == "--user-agent") {
            KeyValue kv; kv.enabled = true; kv.key = "User-Agent"; kv.value = val(i); h.headers.push_back(kv);
        } else if (flag == "-e" || flag == "--referer") {
            KeyValue kv; kv.enabled = true; kv.key = "Referer"; kv.value = val(i); h.headers.push_back(kv);
        } else if (flag == "-b" || flag == "--cookie") {
            KeyValue kv; kv.enabled = true; kv.key = "Cookie"; kv.value = val(i); h.headers.push_back(kv);
        } else if (flag == "-I" || flag == "--head") {
            h.method = "HEAD"; explicitMethod = true;
        } else if (flag == "--compressed") {
            KeyValue kv; kv.enabled = true; kv.key = "Accept-Encoding"; kv.value = "gzip, deflate"; h.headers.push_back(kv);
        } else if (flag == "-m" || flag == "--max-time" || flag == "--connect-timeout") {
            try { h.settings.timeoutMs = (int)(std::stod(val(i)) * 1000); h.settings.timeoutMsSet = true; } catch (...) {}
        } else if (flag == "--data-urlencode") {
            std::string kvs = val(i);
            size_t eq = kvs.find('=');
            KeyValue kv;
            if (eq == std::string::npos) kv.value = kvs;
            else { kv.key = kvs.substr(0, eq); kv.value = kvs.substr(eq + 1); }
            urlEncodeParts.push_back(kv);
        } else if (flag == "-F" || flag == "--form") {
            std::string f = val(i);
            size_t eq = f.find('=');
            MultipartPart p;
            p.key = eq == std::string::npos ? f : f.substr(0, eq);
            std::string v = eq == std::string::npos ? "" : f.substr(eq + 1);
            if (!v.empty() && v.front() == '@') { p.type = "file"; p.filePath = v.substr(1); }
            else { p.type = "text"; p.value = v; }
            h.body.multipart.push_back(p);
            h.body.mode = "multipart";
        } else if (flag == "-G" || flag == "--get") {
            getStyle = true;
        } else if (flag == "-L" || flag == "--location") {
            h.settings.followRedirects = true; h.settings.followRedirectsSet = true;
        } else if (flag == "-k" || flag == "--insecure") {
            h.settings.verifyTls = false; h.settings.verifyTlsSet = true;
        } else if (flag == "--url") {
            h.url = val(i);
        } else if (flag == "-s" || flag == "--silent" ||
                   flag == "-i" || flag == "--include" || flag == "-v" || flag == "--verbose" ||
                   flag == "-#" || flag == "--progress-bar" ||
                   flag == "-o" || flag == "--output" || flag == "-O" || flag == "--remote-name") {
            if (flag == "-o" || flag == "--output") (void)val(i); // consume output value, ignore
            // ignore: output/log flags, no effect on request model
        } else if (!tk.empty() && tk[0] == '-') {
            res.unknown.push_back(tk); // unknown flag -> collect
        } else {
            // bare token = URL (take the first)
            if (h.url.empty()) h.url = tk;
            else res.unknown.push_back(tk);
        }
    }

    // Merge data.
    if (!dataParts.empty()) {
        std::string joined;
        for (size_t i = 0; i < dataParts.size(); ++i) {
            if (i) joined += "&";
            joined += dataParts[i];
        }
        if (getStyle) {
            // -G: data becomes query string -> params (split k=v by &)
            size_t pos = 0;
            while (pos < joined.size()) {
                size_t amp = joined.find('&', pos);
                std::string seg = joined.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                size_t eq = seg.find('=');
                KeyValue kv;
                kv.key = eq == std::string::npos ? seg : seg.substr(0, eq);
                kv.value = eq == std::string::npos ? "" : seg.substr(eq + 1);
                h.params.push_back(kv);
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        } else if (contentType.find("application/x-www-form-urlencoded") != std::string::npos) {
            h.body.mode = "form-urlencoded";
            size_t pos = 0;
            while (pos < joined.size()) {
                size_t amp = joined.find('&', pos);
                std::string seg = joined.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                size_t eq = seg.find('=');
                KeyValue kv;
                kv.key = eq == std::string::npos ? seg : seg.substr(0, eq);
                kv.value = eq == std::string::npos ? "" : seg.substr(eq + 1);
                h.body.formUrlEncoded.push_back(kv);
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        } else if (contentType.find("application/json") != std::string::npos || isJsonLike(joined)) {
            h.body.mode = "json";
            h.body.json = joined;
        } else {
            h.body.mode = "text";
            h.body.text = joined;
        }
    }

    if (!urlEncodeParts.empty() && h.body.mode != "form-urlencoded") {
        h.body.mode = "form-urlencoded";
        for (auto& kv : urlEncodeParts) h.body.formUrlEncoded.push_back(kv);
    }

    // Infer method if not declared: body -> POST, otherwise GET.
    if (!explicitMethod) {
        bool hasBody = h.body.mode != "none";
        h.method = hasBody ? "POST" : "GET";
    }

    if (h.url.empty()) {
        res.error = "no URL found in cURL command";
        return res;
    }

    // Query after '?' -> split into params (decoded), remaining url is raw.
    urlutil::splitUrlQuery(h.url, h.params);

    // Timeout + TLS live in the per-request Config (RequestConfig). Carry imported intent across.
    if (h.settings.timeoutMsSet) m.config.timeoutMs = h.settings.timeoutMs;
    if (h.settings.verifyTlsSet) m.config.tls = h.settings.verifyTls;

    res.ok = true;
    res.model = std::move(m);
    return res;
}

} // namespace core
