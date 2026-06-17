#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "core/importer.hpp"
#include "shell_tokenize.hpp"

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

// Tách "key: value" của -H.
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

} // namespace

bool CurlImporter::canHandle(const std::string& input) const {
    std::string t = lower(trim(input));
    return t.rfind("curl", 0) == 0;
}

ImportResult CurlImporter::parse(const std::string& input) const {
    ImportResult res;
    auto tokens = shellTokenize(input);
    if (tokens.empty() || lower(tokens[0]) != "curl") {
        res.error = "không phải lệnh cURL";
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
        // hỗ trợ --flag=value
        if (tk.rfind("--", 0) == 0) {
            size_t eq = tk.find('=');
            if (eq != std::string::npos) { flag = tk.substr(0, eq); inlineVal = tk.substr(eq + 1); }
        }
        auto val = [&](size_t& idx) { return !inlineVal.empty() ? inlineVal : nextArg(idx); };

        if (flag == "-X" || flag == "--request") {
            h.method = val(i); explicitMethod = true;
        } else if (flag == "-H" || flag == "--header") {
            KeyValue kv = parseHeader(val(i));
            if (lower(kv.key) == "content-type") contentType = lower(kv.value);
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
            if (flag == "-o" || flag == "--output") (void)val(i); // nuốt giá trị output, bỏ qua
            // bỏ qua: flag output/log, không ảnh hưởng request model
        } else if (!tk.empty() && tk[0] == '-') {
            res.unknown.push_back(tk); // cờ lạ -> gom
        } else {
            // bare token = URL (lấy cái đầu)
            if (h.url.empty()) h.url = tk;
            else res.unknown.push_back(tk);
        }
    }

    // Gộp data.
    if (!dataParts.empty()) {
        std::string joined;
        for (size_t i = 0; i < dataParts.size(); ++i) {
            if (i) joined += "&";
            joined += dataParts[i];
        }
        if (getStyle) {
            // -G: data thành query string -> params (tách k=v theo &)
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

    // Suy method nếu không khai báo: có body -> POST, ngược lại GET.
    if (!explicitMethod) {
        bool hasBody = h.body.mode != "none";
        h.method = hasBody ? "POST" : "GET";
    }

    if (h.url.empty()) {
        res.error = "không tìm thấy URL trong lệnh cURL";
        return res;
    }

    res.ok = true;
    res.model = std::move(m);
    return res;
}

} // namespace core
