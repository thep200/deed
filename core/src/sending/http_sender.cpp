#include "sending/http_sender.hpp"

#include <chrono>
#include <thread>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "codec/json_codec.hpp"
#include "core/streaming/sse_parser.hpp"
#include "infra/url_util.hpp"

namespace core {

namespace {

// Replace :id in the path with pathVariables (already resolved {{var}}).
std::string applyPathVariables(std::string url, const std::vector<KeyValue>& vars) {
    for (const auto& v : vars) {
        if (!v.enabled || v.key.empty()) continue;
        std::string token = ":" + v.key;
        size_t pos = 0;
        while ((pos = url.find(token, pos)) != std::string::npos) {
            // only replace when token is followed by '/', '?', or end of string (avoid :id matching :identifier)
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

// Minimal Set-Cookie header parse (POC: display only, no jar). README §12.4.
Cookie parseSetCookie(const std::string& raw) {
    Cookie c;
    size_t i = 0;
    // leading "name=value" part
    size_t semi = raw.find(';');
    std::string first = raw.substr(0, semi);
    size_t eq = first.find('=');
    if (eq != std::string::npos) {
        c.name = first.substr(0, eq);
        c.value = first.substr(eq + 1);
    }
    // the attributes
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

// Configure a cpr::Session from the resolved HTTP request (shared by send() + openStream()). Returns
// false + err on a fatal local error (binary file open). `extraHeaders` are merged last (SSE adds
// Accept / Last-Event-ID). Cancellation is wired via the progress callback.
bool prepareSession(cpr::Session& session, const HttpRequest& h,
                    const std::shared_ptr<CancelToken>& cancel,
                    const std::vector<KeyValue>& extraHeaders, std::string& err) {
    std::string url = applyPathVariables(h.url, h.pathVariables);
    // User may type a query into the URL -> split it out (decode) to send correctly, url stays raw.
    std::vector<KeyValue> urlQuery;
    urlutil::splitUrlQuery(url, urlQuery);
    session.SetUrl(cpr::Url{url});

    // Query params (from the Query tab + query embedded in the URL). cpr re-encodes them.
    cpr::Parameters params;
    for (const auto& p : h.params) {
        if (p.enabled && !p.key.empty()) params.Add({p.key, p.value});
    }
    for (const auto& p : urlQuery) {
        if (!p.key.empty()) params.Add({p.key, p.value});
    }
    session.SetParameters(params);

    // Headers (auth may override Authorization later)
    cpr::Header header;
    for (const auto& hd : h.headers) {
        if (hd.enabled && !hd.key.empty()) header[hd.key] = hd.value;
    }

    // --- Auth (takes priority over a manual Authorization header — README §7.2) ---
    bool authActive = (h.auth.type != "none" && !h.auth.type.empty());
    if (authActive && hasAuthHeader(h.headers)) {
        // Drop the manual Authorization header, auth wins.
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
    for (const auto& e : extraHeaders)        // SSE: Accept / Last-Event-ID (merged last)
        if (!e.key.empty()) header[e.key] = e.value;
    session.SetHeader(header);

    // --- Body by mode ---
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
        // read the binary file
        FILE* f = std::fopen(b.binaryFilePath.c_str(), "rb");
        if (!f) {
            // Open failed (missing/no permission): report so the caller surfaces "cannot open".
            err = "cannot open binary file: " + b.binaryFilePath;
            return false;
        }
        // §2.2: allocate once based on file size -> avoid repeated reallocs for large files.
        if (std::fseek(f, 0, SEEK_END) == 0) {
            long sz = std::ftell(f);
            if (sz > 0) data.reserve(static_cast<size_t>(sz));
            std::rewind(f);
        }
        char buf[64 * 1024];
        size_t n;
        bool aborted = false;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            if (cancel && cancel->isCancelled()) { aborted = true; break; }  // cancel mid-read of a large file
            data.append(buf, n);
        }
        std::fclose(f);
        if (aborted) { err = "Cancelled"; return false; }
        session.SetBody(cpr::Body{data});
    }

    // Settings
    session.SetTimeout(cpr::Timeout{std::chrono::milliseconds(h.settings.timeoutMs)});
    session.SetRedirect(cpr::Redirect{h.settings.followRedirects ? 50L : 0L});
    session.SetVerifySsl(cpr::VerifySsl{h.settings.verifyTls});

    // Cancellation: cpr progress callback returns false -> cancel.
    session.SetProgressCallback(cpr::ProgressCallback{
        [cancel](cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) -> bool {
            return !(cancel && cancel->isCancelled());
        }});
    return true;
}

// Run the configured session with the request's verb.
cpr::Response runVerb(cpr::Session& session, const std::string& methodIn) {
    std::string method = methodIn.empty() ? "GET" : methodIn;
    if (method == "POST") return session.Post();
    if (method == "PUT") return session.Put();
    if (method == "PATCH") return session.Patch();
    if (method == "DELETE") return session.Delete();
    if (method == "HEAD") return session.Head();
    if (method == "OPTIONS") return session.Options();
    return session.Get();
}

} // namespace

void HttpSender::send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
                      const std::shared_ptr<CancelToken>& cancel) {
    const HttpRequest& h = req.model.http;

    cpr::Session session;
    std::string err;
    if (!prepareSession(session, h, cancel, {}, err)) {
        ErrorKind k = (err == "Cancelled") ? ErrorKind::Cancelled : ErrorKind::Unknown;
        delegate.onError(handle, ApiError{k, err});
        return;
    }

    auto start = std::chrono::steady_clock::now();
    cpr::Response r = runVerb(session, h.method);
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

bool HttpSender::isStreaming(const ResolvedRequest& req) const {
    return httpRequestsSse(req.model.http);
}

// SSE (SPEC_sse §4): stream the HTTP response, feed each chunk to SseParser, emit one StreamEvent per
// event. Reconnects with Last-Event-ID on abnormal end (§7). Auto + non-SSE response -> render the body
// as a single element (no infinite gather; documented degradation of §5).
void HttpSender::openStream(const ResolvedRequest& req, IStreamSink& sink,
                            const std::shared_ptr<CancelToken>& cancel) {
    const HttpRequest& h = req.model.http;
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto offsetMs = [&] {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
    };
    auto epochMs = [] {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };

    // Config (SPEC_sse §10). Defaults; event-size cap reuses the stream byte ceiling if provided.
    const int kRetryDefaultMs = 3000;
    const int kMaxRetries = 10;
    const std::uint64_t kMaxEventBytes = req.streamMaxBytes ? req.streamMaxBytes : 8ull * 1024 * 1024;

    StreamMeta meta;
    meta.streamId = req.streamId;
    meta.transport = StreamTransport::Sse;
    meta.startedAtEpochMs = epochMs();

    bool opened = false;
    std::uint64_t seq = 0, bytes = 0;
    std::string lastEventId;
    long retryMs = kRetryDefaultMs;

    StreamStatus endStatus = StreamStatus::Ok;
    int endCode = 0;
    std::string endMsg;

    for (int attempt = 0;; ++attempt) {
        cpr::Session session;
        std::vector<KeyValue> extra;
        extra.push_back({"Accept", "text/event-stream", true});
        if (!lastEventId.empty()) extra.push_back({"Last-Event-ID", lastEventId, true});

        std::string err;
        if (!prepareSession(session, h, cancel, extra, err)) {
            if (!opened) { sink.onStreamOpen(meta); opened = true; }
            endStatus = (err == "Cancelled") ? StreamStatus::Cancelled : StreamStatus::Error;
            endMsg = err;
            break;
        }
        session.SetAcceptEncoding(cpr::AcceptEncoding{{cpr::AcceptEncodingMethods::deflate,
                                                       cpr::AcceptEncodingMethods::gzip}});

        // Per-connection callback state. Decided on the first body chunk (Content-Type known by then).
        struct CbState {
            int status = 0;
            std::string ctype;
            std::vector<KeyValue> leading;
            bool decided = false;
            bool isSse = false;
        };
        auto st = std::make_shared<CbState>();

        session.SetHeaderCallback(cpr::HeaderCallback{[st](std::string_view line, intptr_t) -> bool {
            std::string s(line);
            if (s.rfind("HTTP/", 0) == 0) {            // status line resets the header set (redirects)
                std::size_t sp = s.find(' ');
                if (sp != std::string::npos) st->status = std::atoi(s.c_str() + sp + 1);
                st->leading.clear();
                st->ctype.clear();
            } else {
                std::size_t c = s.find(':');
                if (c != std::string::npos) {
                    std::string k = s.substr(0, c), v = s.substr(c + 1);
                    auto trim = [](std::string& x) {
                        std::size_t a = x.find_first_not_of(" \t\r\n");
                        std::size_t b = x.find_last_not_of(" \t\r\n");
                        x = (a == std::string::npos) ? std::string() : x.substr(a, b - a + 1);
                    };
                    trim(k); trim(v);
                    if (!k.empty()) st->leading.push_back({k, v, true});
                    std::string lk = k;
                    for (auto& ch : lk) ch = static_cast<char>(::tolower(ch));
                    if (lk == "content-type") { st->ctype = v; for (auto& ch : st->ctype) ch = static_cast<char>(::tolower(ch)); }
                }
            }
            return true;
        }});

        SseParser parser;
        parser.setMaxEventBytes(static_cast<std::size_t>(kMaxEventBytes));

        // Build one log element per SSE event: {"event","id","data"} (data = JSON if parseable, else string).
        auto onEvent = [&](const SseEvent& e) {
            nlohmann::json env;
            env["event"] = e.event.empty() ? std::string("message") : e.event;
            if (!e.id.empty()) env["id"] = e.id;
            try { env["data"] = nlohmann::json::parse(e.data); }
            catch (...) { env["data"] = e.data; }
            std::string payload = env.dump();
            StreamEvent ev;
            ev.seq = seq;
            ev.direction = StreamDirection::Inbound;
            ev.frameType = StreamFrameType::Message;
            ev.kind = StreamPayloadKind::Json;
            ev.payload = payload;
            ev.name = e.event;
            ev.id = e.id;
            ev.offsetMs = offsetMs();
            sink.onStreamEvent(ev);
            ++seq;
            bytes += payload.size();
            if (!e.id.empty()) lastEventId = e.id;
        };

        std::string unaryBody;   // Auto + non-SSE: accumulate to show as one element
        session.SetWriteCallback(cpr::WriteCallback{
            [&, st](std::string_view data, intptr_t) -> bool {
                if (cancel && cancel->cancelled()) return false;   // abort perform
                if (!st->decided) {
                    st->decided = true;
                    st->isSse = httpForcesSse(h) ||
                                (st->ctype.find("text/event-stream") != std::string::npos);
                    if (!opened) { meta.leading = st->leading; sink.onStreamOpen(meta); opened = true; }
                }
                if (st->isSse) parser.feed(data.data(), data.size(), onEvent);
                else unaryBody.append(data);
                return true;
            }});

        cpr::Response r = runVerb(session, h.method);
        if (!opened) { meta.leading = st->leading; sink.onStreamOpen(meta); opened = true; }
        if (parser.retryMs() >= 0) retryMs = parser.retryMs();   // server-provided backoff

        if (cancel && cancel->cancelled()) { endStatus = StreamStatus::Cancelled; endCode = st->status; break; }

        // Non-SSE under Auto: emit the gathered body as one element, then finish (no reconnect).
        if (st->decided && !st->isSse) {
            if (!unaryBody.empty()) {
                StreamEvent ev;
                ev.seq = seq++;
                ev.payload = unaryBody;
                ev.kind = StreamPayloadKind::Text;
                ev.offsetMs = offsetMs();
                sink.onStreamEvent(ev);
            }
            endStatus = StreamStatus::Ok;
            endCode = (int)r.status_code;
            break;
        }

        // Fatal HTTP for SSE: 204 / 4xx / 5xx / explicit-Sse-but-wrong-content-type -> no reconnect (§7).
        bool fatalHttp = (st->status == 204) || (st->status >= 400) ||
                         (httpForcesSse(h) && st->decided && !st->isSse);
        if (r.error && r.error.code != cpr::ErrorCode::OK) {
            // Network drop -> reconnect with Last-Event-ID, up to the cap.
            if (attempt < kMaxRetries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(retryMs));
                if (cancel && cancel->cancelled()) { endStatus = StreamStatus::Cancelled; break; }
                continue;
            }
            endStatus = StreamStatus::Error;
            endCode = (int)r.status_code;
            endMsg = r.error.message;
            break;
        }
        if (fatalHttp) {
            endStatus = StreamStatus::Error;
            endCode = st->status ? st->status : (int)r.status_code;
            endMsg = "SSE: unexpected HTTP status / content-type";
            break;
        }
        // Clean server close on a 2xx SSE stream -> reconnect (EventSource semantics), up to the cap.
        if (attempt < kMaxRetries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryMs));
            if (cancel && cancel->cancelled()) { endStatus = StreamStatus::Cancelled; break; }
            continue;
        }
        endStatus = StreamStatus::Ok;
        endCode = (int)r.status_code;
        break;
    }

    if (!opened) sink.onStreamOpen(meta);
    StreamEnd end;
    end.status = endStatus;
    end.statusCode = endCode;
    end.statusMessage = endMsg;
    end.totalEvents = seq;
    end.totalBytes = bytes;
    end.elapsedMs = offsetMs();
    sink.onStreamClose(end);
}

} // namespace core
