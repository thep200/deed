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

// Apply the request's auth to the cpr header/params/session. Auth wins over a manual Authorization
// header (README §7.2): basic -> SetAuth, bearer/apikey-header -> header, apikey-query -> params.
void applyHttpAuth(cpr::Session& session, cpr::Header& header, cpr::Parameters& params,
                   const HttpRequest& h) {
    bool authActive = (h.auth.type != "none" && !h.auth.type.empty());
    if (authActive && hasAuthHeader(h.headers)) {   // drop the manual Authorization header — auth wins
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
}

// Read a binary upload file into `data`, honoring cancel mid-read of a large file. false + err on
// open failure ("cannot open...") or cancel ("Cancelled").
bool readBinaryFile(const std::string& path, const std::shared_ptr<CancelToken>& cancel,
                    std::string& data, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open binary file: " + path; return false; }
    if (std::fseek(f, 0, SEEK_END) == 0) {   // §2.2: reserve once based on size to avoid realloc churn
        long sz = std::ftell(f);
        if (sz > 0) data.reserve(static_cast<size_t>(sz));
        std::rewind(f);
    }
    char buf[64 * 1024];
    size_t n;
    bool aborted = false;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        if (cancel && cancel->isCancelled()) { aborted = true; break; }
        data.append(buf, n);
    }
    std::fclose(f);
    if (aborted) { err = "Cancelled"; return false; }
    return true;
}

// GraphQL-over-HTTP body: {"query": ..., "variables": ...} (variables parsed as JSON, else kept raw).
std::string graphqlBody(const Body& b) {
    nlohmann::json g;
    g["query"] = b.graphqlQuery;
    if (!b.graphqlVariables.empty()) {
        try { g["variables"] = nlohmann::json::parse(b.graphqlVariables); }
        catch (...) { g["variables"] = b.graphqlVariables; }
    }
    return g.dump();
}

void applyFormBody(cpr::Session& session, const Body& b) {
    cpr::Payload payload{};
    for (const auto& kv : b.formUrlEncoded)
        if (kv.enabled) payload.Add({kv.key, kv.value});
    session.SetPayload(payload);
}

void applyMultipartBody(cpr::Session& session, const Body& b) {
    cpr::Multipart mp{};
    for (const auto& part : b.multipart) {
        if (!part.enabled) continue;
        if (part.type == "file") mp.parts.emplace_back(part.key, cpr::File{part.filePath});
        else mp.parts.emplace_back(part.key, part.value);
    }
    session.SetMultipart(mp);
}

// Set the cpr request body for the request's body mode. false + err only on a fatal local error
// (binary file open / cancel during read).
bool applyRequestBody(cpr::Session& session, const HttpRequest& h,
                      const std::shared_ptr<CancelToken>& cancel, std::string& err) {
    const Body& b = h.body;
    if (b.mode == "json") session.SetBody(cpr::Body{b.json});
    else if (b.mode == "text") session.SetBody(cpr::Body{b.text});
    else if (b.mode == "xml") session.SetBody(cpr::Body{b.xml});
    else if (b.mode == "graphql") session.SetBody(cpr::Body{graphqlBody(b)});
    else if (b.mode == "form-urlencoded") applyFormBody(session, b);
    else if (b.mode == "multipart") applyMultipartBody(session, b);
    else if (b.mode == "binary") {
        std::string data;
        if (!readBinaryFile(b.binaryFilePath, cancel, data, err)) return false;
        session.SetBody(cpr::Body{data});
    }
    return true;
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
    applyHttpAuth(session, header, params, h);
    for (const auto& e : extraHeaders)        // SSE: Accept / Last-Event-ID (merged last)
        if (!e.key.empty()) header[e.key] = e.value;
    session.SetHeader(header);

    // --- Body by mode ---
    if (!applyRequestBody(session, h, cancel, err)) return false;

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

long long epochMs() {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void trimWs(std::string& x) {
    std::size_t a = x.find_first_not_of(" \t\r\n");
    std::size_t b = x.find_last_not_of(" \t\r\n");
    x = (a == std::string::npos) ? std::string() : x.substr(a, b - a + 1);
}

// SSE streaming driver (SPEC_sse §4/§7). Owns the reconnect loop + all cross-attempt state so the
// per-connection callbacks (header / write) and the post-perform decision stay as small methods rather
// than deeply nested lambdas. Drives one HTTP transfer per attempt; reconnects with Last-Event-ID.
class SseStreamer {
public:
    // Config (§10): event-size cap reuses the stream byte ceiling; total ceilings mirror the gRPC path
    // (M11/H4) so a long/reconnecting stream — or a misclassified infinite non-SSE body under Auto —
    // can't grow RAM without bound.
    SseStreamer(const ResolvedRequest& req, IStreamSink& sink, std::shared_ptr<CancelToken> cancel)
        : req_(req), h_(req.model.http), sink_(sink), cancel_(std::move(cancel)),
          t0_(std::chrono::steady_clock::now()),
          kMaxEventBytes_(req.streamMaxBytes ? req.streamMaxBytes : 8ull * 1024 * 1024),
          kMaxEvents_(req.streamMaxEvents ? req.streamMaxEvents : 100000),
          kMaxTotalBytes_(req.streamMaxBytes ? req.streamMaxBytes : 64ull * 1024 * 1024) {
        meta_.streamId = req.streamId;
        meta_.transport = StreamTransport::Sse;
        meta_.startedAtEpochMs = epochMs();
    }

    void run() {
        for (int attempt = 0;; ++attempt)
            if (runAttempt(attempt) == Step::Stop) break;
        openBare();   // §3: ensure exactly one open even if we never received a chunk
        StreamEnd end;
        end.status = endStatus_;
        end.statusCode = endCode_;
        end.statusMessage = endMsg_;
        end.totalEvents = seq_;
        end.totalBytes = bytes_;
        end.elapsedMs = offsetMs();
        end.truncated = truncated_;
        sink_.onStreamClose(end);
    }

private:
    enum class Step { Reconnect, Stop };
    enum class Retry { DoReconnect, Cancelled, CapReached };

    // Per-connection state, decided on the first body chunk (Content-Type known by then).
    struct Conn {
        int status = 0;
        std::string ctype;
        std::vector<KeyValue> leading;
        bool decided = false;
        bool isSse = false;
        std::string unaryBody;   // Auto + non-SSE: accumulate to show as one element
    };

    long long offsetMs() const {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0_).count());
    }
    void openBare() { if (!opened_) { sink_.onStreamOpen(meta_); opened_ = true; } }
    void openWithLeading(const Conn& conn) {
        if (opened_) return;
        meta_.leading = conn.leading;
        sink_.onStreamOpen(meta_);
        opened_ = true;
    }

    // One completed SSE event -> one log element {"event","id","data"} (data = JSON if parseable, else string).
    void emitEvent(const SseEvent& e) {
        nlohmann::json env;
        env["event"] = e.event.empty() ? std::string("message") : e.event;
        if (!e.id.empty()) env["id"] = e.id;
        try { env["data"] = nlohmann::json::parse(e.data); }
        catch (...) { env["data"] = e.data; }
        std::string payload = env.dump();
        StreamEvent ev;
        ev.seq = seq_;
        ev.direction = StreamDirection::Inbound;
        ev.frameType = StreamFrameType::Message;
        ev.kind = StreamPayloadKind::Json;
        ev.payload = payload;
        ev.name = e.event;
        ev.id = e.id;
        ev.offsetMs = offsetMs();
        sink_.onStreamEvent(ev);
        ++seq_;
        bytes_ += payload.size();
        if (!e.id.empty()) lastEventId_ = e.id;
    }

    void emitUnaryBody(const std::string& body) {
        StreamEvent ev;
        ev.seq = seq_++;
        ev.payload = body;
        ev.kind = StreamPayloadKind::Text;
        ev.offsetMs = offsetMs();
        sink_.onStreamEvent(ev);
    }

    // cpr header callback body: track status line (resets headers on redirects) + leading headers + ctype.
    void onHeaderLine(Conn& conn, std::string_view line) {
        std::string s(line);
        if (s.rfind("HTTP/", 0) == 0) {            // status line resets the header set (redirects)
            std::size_t sp = s.find(' ');
            if (sp != std::string::npos) conn.status = std::atoi(s.c_str() + sp + 1);
            conn.leading.clear();
            conn.ctype.clear();
            return;
        }
        std::size_t c = s.find(':');
        if (c == std::string::npos) return;
        std::string k = s.substr(0, c), v = s.substr(c + 1);
        trimWs(k);
        trimWs(v);
        if (!k.empty()) conn.leading.push_back({k, v, true});
        std::string lk = k;
        for (auto& ch : lk) ch = static_cast<char>(::tolower(ch));
        if (lk == "content-type") { conn.ctype = v; for (auto& ch : conn.ctype) ch = static_cast<char>(::tolower(ch)); }
    }

    // cpr write callback body. Decides SSE-vs-unary on the first chunk; feeds the parser or gathers the
    // body, enforcing the total ceilings. Returns false to abort the transfer (cancel / ceiling).
    bool onChunk(Conn& conn, SseParser& parser, const SseParser::Emit& onEvent, std::string_view data) {
        if (cancel_ && cancel_->cancelled()) return false;
        if (truncated_) return false;
        if (!conn.decided) {
            conn.decided = true;
            conn.isSse = httpForcesSse(h_) || (conn.ctype.find("text/event-stream") != std::string::npos);
            openWithLeading(conn);
        }
        if (conn.isSse) {
            parser.feed(data.data(), data.size(), onEvent);
            if (seq_ >= kMaxEvents_ || bytes_ >= kMaxTotalBytes_) { truncated_ = true; return false; }
            return true;
        }
        // H4: bound the gathered body — never append past the byte ceiling.
        if (conn.unaryBody.size() >= kMaxTotalBytes_) { truncated_ = true; return false; }
        std::size_t room = static_cast<std::size_t>(kMaxTotalBytes_ - conn.unaryBody.size());
        conn.unaryBody.append(data.substr(0, room));
        if (conn.unaryBody.size() >= kMaxTotalBytes_) { truncated_ = true; return false; }
        return true;
    }

    // Sleep the backoff, then report whether to reconnect (cap reached / cancelled during the sleep).
    Retry tryReconnect(int attempt) {
        if (attempt >= kMaxRetries) return Retry::CapReached;
        std::this_thread::sleep_for(std::chrono::milliseconds(retryMs_));
        if (cancel_ && cancel_->cancelled()) { endStatus_ = StreamStatus::Cancelled; return Retry::Cancelled; }
        return Retry::DoReconnect;
    }

    // Decide what to do after one transfer finishes: emit the unary body, stop, or reconnect (§7).
    Step decideAfterPerform(int attempt, Conn& conn, const cpr::Response& r) {
        if (cancel_ && cancel_->cancelled()) { endStatus_ = StreamStatus::Cancelled; endCode_ = conn.status; return Step::Stop; }
        if (conn.decided && !conn.isSse) {                 // Auto + non-SSE: emit gathered body, finish (no reconnect)
            if (!conn.unaryBody.empty()) emitUnaryBody(conn.unaryBody);
            endStatus_ = StreamStatus::Ok; endCode_ = static_cast<int>(r.status_code); return Step::Stop;
        }
        if (truncated_) { endStatus_ = StreamStatus::Ok; endCode_ = static_cast<int>(r.status_code); return Step::Stop; }
        // Fatal HTTP for SSE: 204 / 4xx / 5xx / explicit-Sse-but-wrong-content-type -> no reconnect (§7).
        bool fatalHttp = (conn.status == 204) || (conn.status >= 400) ||
                         (httpForcesSse(h_) && conn.decided && !conn.isSse);
        if (r.error && r.error.code != cpr::ErrorCode::OK) {   // network drop -> reconnect up to the cap
            switch (tryReconnect(attempt)) {
                case Retry::DoReconnect: return Step::Reconnect;
                case Retry::Cancelled: return Step::Stop;
                case Retry::CapReached: break;
            }
            endStatus_ = StreamStatus::Error; endCode_ = static_cast<int>(r.status_code); endMsg_ = r.error.message;
            return Step::Stop;
        }
        if (fatalHttp) {
            endStatus_ = StreamStatus::Error;
            endCode_ = conn.status ? conn.status : static_cast<int>(r.status_code);
            endMsg_ = "SSE: unexpected HTTP status / content-type";
            return Step::Stop;
        }
        // Clean server close on a 2xx SSE stream -> reconnect (EventSource semantics), up to the cap.
        switch (tryReconnect(attempt)) {
            case Retry::DoReconnect: return Step::Reconnect;
            case Retry::Cancelled: return Step::Stop;
            case Retry::CapReached: break;
        }
        endStatus_ = StreamStatus::Ok; endCode_ = static_cast<int>(r.status_code);
        return Step::Stop;
    }

    // One connection attempt: configure the session + callbacks, perform, then decide next step.
    Step runAttempt(int attempt) {
        cpr::Session session;
        std::vector<KeyValue> extra;
        extra.push_back({"Accept", "text/event-stream", true});
        if (!lastEventId_.empty()) extra.push_back({"Last-Event-ID", lastEventId_, true});

        std::string err;
        if (!prepareSession(session, h_, cancel_, extra, err)) {
            openBare();
            endStatus_ = (err == "Cancelled") ? StreamStatus::Cancelled : StreamStatus::Error;
            endMsg_ = err;
            return Step::Stop;
        }
        session.SetAcceptEncoding(cpr::AcceptEncoding{{cpr::AcceptEncodingMethods::deflate,
                                                       cpr::AcceptEncodingMethods::gzip}});

        Conn conn;
        SseParser parser;
        parser.setMaxEventBytes(static_cast<std::size_t>(kMaxEventBytes_));
        SseParser::Emit onEvent = [this](const SseEvent& e) { emitEvent(e); };

        session.SetHeaderCallback(cpr::HeaderCallback{
            [this, &conn](std::string_view line, intptr_t) -> bool { onHeaderLine(conn, line); return true; }});
        session.SetWriteCallback(cpr::WriteCallback{
            [this, &conn, &parser, &onEvent](std::string_view data, intptr_t) -> bool {
                return onChunk(conn, parser, onEvent, data);
            }});

        cpr::Response r = runVerb(session, h_.method);
        openWithLeading(conn);
        if (conn.decided && conn.isSse && !truncated_) parser.finish(onEvent);   // M12: flush a final event
        if (parser.retryMs() >= 0) retryMs_ = parser.retryMs();                   // server-provided backoff
        return decideAfterPerform(attempt, conn, r);
    }

    static constexpr int kRetryDefaultMs = 3000;
    static constexpr int kMaxRetries = 10;

    const ResolvedRequest& req_;
    const HttpRequest& h_;
    IStreamSink& sink_;
    std::shared_ptr<CancelToken> cancel_;
    std::chrono::steady_clock::time_point t0_;

    std::uint64_t kMaxEventBytes_ = 0;
    std::uint64_t kMaxEvents_ = 0;
    std::uint64_t kMaxTotalBytes_ = 0;
    StreamMeta meta_;
    bool opened_ = false;
    bool truncated_ = false;   // a total ceiling was hit -> stop the transfer (H4/M11)
    std::uint64_t seq_ = 0;
    std::uint64_t bytes_ = 0;
    std::string lastEventId_;
    long retryMs_ = kRetryDefaultMs;
    StreamStatus endStatus_ = StreamStatus::Ok;
    int endCode_ = 0;
    std::string endMsg_;
};

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
    resp.sizeBytes = static_cast<std::int64_t>(r.text.size());
    resp.body = std::move(r.text);   // r is a local cpr::Response -> move the (possibly large) body (L10)
    resp.elapsedMs = static_cast<long>(elapsed);
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
    SseStreamer(req, sink, cancel).run();
}

} // namespace core
