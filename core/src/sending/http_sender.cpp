#include "sending/http_sender.hpp"

#include <chrono>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "codec/json_codec.hpp"
#include "infra/url_util.hpp"

namespace core {

namespace {

// Thay :id trong path bằng pathVariables (đã resolve {{var}}).
std::string applyPathVariables(std::string url, const std::vector<KeyValue>& vars) {
    for (const auto& v : vars) {
        if (!v.enabled || v.key.empty()) continue;
        std::string token = ":" + v.key;
        size_t pos = 0;
        while ((pos = url.find(token, pos)) != std::string::npos) {
            // chỉ thay khi sau token là '/', '?', hết chuỗi (tránh :id khớp :identifier)
            size_t end = pos + token.size();
            char nxt = end < url.size() ? url[end] : '/';
            if (nxt == '/' || nxt == '?' || nxt == '&' || end == url.size()) {
                url.replace(pos, token.size(), v.value);
                pos += v.value.size();
            } else {
                pos = end;
            }
        }
    }
    return url;
}

bool hasAuthHeader(const std::vector<KeyValue>& headers) {
    for (const auto& h : headers) {
        if (!h.enabled) continue;
        std::string k = h.key;
        for (auto& c : k) c = static_cast<char>(::tolower(c));
        if (k == "authorization") return true;
    }
    return false;
}

ErrorKind mapCprError(cpr::ErrorCode code) {
    switch (code) {
        case cpr::ErrorCode::OPERATION_TIMEDOUT: return ErrorKind::Timeout;
        case cpr::ErrorCode::ABORTED_BY_CALLBACK: return ErrorKind::Cancelled;
        case cpr::ErrorCode::SSL_CONNECT_ERROR:
        case cpr::ErrorCode::SSL_CERTPROBLEM:
        case cpr::ErrorCode::SSL_CACERT_BADFILE:
        case cpr::ErrorCode::PEER_FAILED_VERIFICATION:
        case cpr::ErrorCode::USE_SSL_FAILED: return ErrorKind::Tls;
        default: return ErrorKind::Network;
    }
}

// Parse Set-Cookie header tối giản (POC: hiển thị, không jar). README §12.4.
Cookie parseSetCookie(const std::string& raw) {
    Cookie c;
    size_t i = 0;
    // phần đầu "name=value"
    size_t semi = raw.find(';');
    std::string first = raw.substr(0, semi);
    size_t eq = first.find('=');
    if (eq != std::string::npos) {
        c.name = first.substr(0, eq);
        c.value = first.substr(eq + 1);
    }
    // các attribute
    while (semi != std::string::npos) {
        size_t next = raw.find(';', semi + 1);
        std::string attr = raw.substr(semi + 1, (next == std::string::npos ? raw.size() : next) - semi - 1);
        // trim
        size_t a = attr.find_first_not_of(" \t");
        if (a != std::string::npos) attr = attr.substr(a);
        std::string lower = attr;
        for (auto& ch : lower) ch = static_cast<char>(::tolower(ch));
        auto valOf = [&](const std::string& s) {
            size_t e = s.find('=');
            return e == std::string::npos ? std::string() : s.substr(e + 1);
        };
        if (lower.rfind("domain=", 0) == 0) c.domain = valOf(attr);
        else if (lower.rfind("path=", 0) == 0) c.path = valOf(attr);
        else if (lower.rfind("expires=", 0) == 0) c.expires = valOf(attr);
        semi = next;
    }
    (void)i;
    return c;
}

} // namespace

void HttpSender::send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
                      const std::shared_ptr<CancelToken>& cancel) {
    const HttpRequest& h = req.model.http;

    cpr::Session session;

    std::string url = applyPathVariables(h.url, h.pathVariables);
    // Người dùng có thể tự gõ query vào URL -> tách ra (decode) để gửi đúng, url còn raw.
    std::vector<KeyValue> urlQuery;
    urlutil::splitUrlQuery(url, urlQuery);
    session.SetUrl(cpr::Url{url});

    // Query params (từ tab Query + query lẫn trong URL). cpr tự encode lại.
    cpr::Parameters params;
    for (const auto& p : h.params) {
        if (p.enabled && !p.key.empty()) params.Add({p.key, p.value});
    }
    for (const auto& p : urlQuery) {
        if (!p.key.empty()) params.Add({p.key, p.value});
    }
    session.SetParameters(params);

    // Headers (auth có thể ghi đè Authorization sau)
    cpr::Header header;
    for (const auto& hd : h.headers) {
        if (hd.enabled && !hd.key.empty()) header[hd.key] = hd.value;
    }

    // --- Auth (ưu tiên hơn header Authorization thủ công — README §7.2) ---
    bool authActive = (h.auth.type != "none" && !h.auth.type.empty());
    if (authActive && hasAuthHeader(h.headers)) {
        // Bỏ header Authorization thủ công, auth thắng.
        for (auto it = header.begin(); it != header.end();) {
            std::string k = it->first;
            for (auto& c : k) c = static_cast<char>(::tolower(c));
            if (k == "authorization") it = header.erase(it);
            else ++it;
        }
    }
    if (h.auth.type == "bearer") {
        header["Authorization"] = "Bearer " + h.auth.bearerToken;
    } else if (h.auth.type == "basic") {
        session.SetAuth(cpr::Authentication{h.auth.basicUsername, h.auth.basicPassword,
                                            cpr::AuthMode::BASIC});
    } else if (h.auth.type == "apikey") {
        if (h.auth.apikeyIn == "query") {
            params.Add({h.auth.apikeyKey, h.auth.apikeyValue});
            session.SetParameters(params);
        } else {
            header[h.auth.apikeyKey] = h.auth.apikeyValue;
        }
    }
    session.SetHeader(header);

    // --- Body theo mode ---
    const Body& b = h.body;
    if (b.mode == "json") {
        session.SetBody(cpr::Body{b.json});
    } else if (b.mode == "text" ) {
        session.SetBody(cpr::Body{b.text});
    } else if (b.mode == "xml") {
        session.SetBody(cpr::Body{b.xml});
    } else if (b.mode == "graphql") {
        nlohmann::json g;
        g["query"] = b.graphqlQuery;
        if (!b.graphqlVariables.empty()) {
            try { g["variables"] = nlohmann::json::parse(b.graphqlVariables); }
            catch (...) { g["variables"] = b.graphqlVariables; }
        }
        session.SetBody(cpr::Body{g.dump()});
    } else if (b.mode == "form-urlencoded") {
        cpr::Payload payload{};
        for (const auto& kv : b.formUrlEncoded)
            if (kv.enabled) payload.Add({kv.key, kv.value});
        session.SetPayload(payload);
    } else if (b.mode == "multipart") {
        cpr::Multipart mp{};
        for (const auto& part : b.multipart) {
            if (!part.enabled) continue;
            if (part.type == "file")
                mp.parts.emplace_back(part.key, cpr::File{part.filePath});
            else
                mp.parts.emplace_back(part.key, part.value);
        }
        session.SetMultipart(mp);
    } else if (b.mode == "binary") {
        std::string data;
        // đọc file nhị phân
        FILE* f = std::fopen(b.binaryFilePath.c_str(), "rb");
        if (f) {
            // §2.2: cấp phát 1 lần theo kích thước file -> tránh realloc nhiều lần với file lớn.
            if (std::fseek(f, 0, SEEK_END) == 0) {
                long sz = std::ftell(f);
                if (sz > 0) data.reserve(static_cast<size_t>(sz));
                std::rewind(f);
            }
            char buf[64 * 1024];
            size_t n;
            bool aborted = false;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                if (cancel && cancel->isCancelled()) { aborted = true; break; }  // huỷ giữa lúc đọc file lớn
                data.append(buf, n);
            }
            std::fclose(f);
            if (aborted) { delegate.onError(handle, ApiError{ErrorKind::Cancelled, "Cancelled"}); return; }
        }
        session.SetBody(cpr::Body{data});
    }

    // Settings
    session.SetTimeout(cpr::Timeout{std::chrono::milliseconds(h.settings.timeoutMs)});
    session.SetRedirect(cpr::Redirect{h.settings.followRedirects ? 50L : 0L});
    session.SetVerifySsl(cpr::VerifySsl{h.settings.verifyTls});

    // Cancellation: cpr progress callback trả false -> huỷ.
    session.SetProgressCallback(cpr::ProgressCallback{
        [cancel](cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) -> bool {
            return !(cancel && cancel->isCancelled());
        }});

    auto start = std::chrono::steady_clock::now();

    // Chọn verb
    std::string method = h.method.empty() ? "GET" : h.method;
    cpr::Response r;
    if (method == "GET") r = session.Get();
    else if (method == "POST") r = session.Post();
    else if (method == "PUT") r = session.Put();
    else if (method == "PATCH") r = session.Patch();
    else if (method == "DELETE") r = session.Delete();
    else if (method == "HEAD") r = session.Head();
    else if (method == "OPTIONS") r = session.Options();
    else r = session.Get();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    if (cancel && cancel->isCancelled()) {
        delegate.onError(handle, ApiError{ErrorKind::Cancelled, "Cancelled"});
        return;
    }

    if (r.error) {
        delegate.onError(handle, ApiError{mapCprError(r.error.code), r.error.message});
        return;
    }

    ApiResponse resp;
    resp.statusCode = static_cast<int>(r.status_code);
    resp.statusText = r.reason;
    resp.body = r.text;
    resp.elapsedMs = static_cast<long>(elapsed);
    resp.sizeBytes = static_cast<std::int64_t>(r.text.size());
    for (const auto& kv : r.header) {
        resp.headers.push_back({kv.first, kv.second, true});
        std::string k = kv.first;
        for (auto& c : k) c = static_cast<char>(::tolower(c));
        if (k == "set-cookie") resp.cookies.push_back(parseSetCookie(kv.second));
    }
    resp.resolvedRequestDump = codec::dumpRequest(req.model);
    delegate.onResponse(handle, resp);
}

} // namespace core
