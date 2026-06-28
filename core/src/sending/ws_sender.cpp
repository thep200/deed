// ws_sender.cpp — WebSocket transport over libcurl (SPEC_websocket §3/§5/§10).
#include "sending/ws_sender.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace core {

namespace {

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
long long nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Runtime check: was this libcurl built with the ws/wss protocol handlers? (Symbols can exist while the
// scheme is disabled — SPEC_websocket §3.1 / W9.) Cached after the first query.
bool curlHasWebSocket() {
    static const bool has = [] {
        const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
        if (!v || !v->protocols) return false;
        for (const char* const* p = v->protocols; *p; ++p)
            if (std::strcmp(*p, "ws") == 0 || std::strcmp(*p, "wss") == 0) return true;
        return false;
    }();
    return has;
}

std::string base64(const std::uint8_t* data, std::size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out.push_back(T[(v >> 18) & 0x3F]);
        out.push_back(T[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? T[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < n ? T[v & 0x3F] : '=');
    }
    return out;
}

} // namespace

// One queued outbound frame.
struct OutFrame {
    std::vector<std::uint8_t> data;
    unsigned flags;   // CURLWS_TEXT | CURLWS_BINARY | CURLWS_CLOSE | CURLWS_PING
    int closeCode = 1000;
};

struct WsSession {
    WsConfig cfg;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<OutFrame> out;
    std::uint64_t outBytes = 0;
    bool wantClose = false;
    int closeCode = 1000;
    std::string closeReason;
    std::atomic<bool> open{false};
    std::atomic<bool> done{false};
};

namespace {

// Enqueue an outbound frame; false if the send queue is full (backpressure §11) or the session is gone.
bool enqueue(const std::shared_ptr<WsSession>& s, OutFrame f) {
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->done.load() || s->wantClose) return false;
    if (s->out.size() >= s->cfg.sendQueueMaxFrames ||
        s->outBytes + f.data.size() > s->cfg.sendQueueMaxBytes)
        return false;
    s->outBytes += f.data.size();
    s->out.push_back(std::move(f));
    s->cv.notify_all();
    return true;
}

// Build the neutral per-frame log element (SPEC §6): {"dir","type","ts","data"|"data_base64"}.
std::string frameEnvelope(StreamDirection dir, bool binary, long long offMs,
                          const std::uint8_t* data, std::size_t n) {
    nlohmann::json env;
    env["dir"] = (dir == StreamDirection::Outbound) ? "out" : "in";
    env["type"] = binary ? "binary" : "text";
    env["ts"] = offMs;
    if (binary) {
        env["data_base64"] = base64(data, n);
    } else {
        std::string text(reinterpret_cast<const char*>(data), n);
        try {
            env["data"] = nlohmann::json::parse(text);   // embed parsed JSON when possible
        } catch (...) {
            env["data"] = text;                          // else raw string
        }
    }
    return env.dump();
}

class WsChannel : public IStreamChannel {
public:
    explicit WsChannel(std::shared_ptr<WsSession> s) : s_(std::move(s)) {}
    bool sendText(const std::string& utf8) override {
        return enqueue(s_, OutFrame{std::vector<std::uint8_t>(utf8.begin(), utf8.end()),
                                    CURLWS_TEXT, 0});
    }
    bool sendBinary(const std::vector<std::uint8_t>& bytes) override {
        return enqueue(s_, OutFrame{bytes, CURLWS_BINARY, 0});
    }
    void close(int code, const std::string& reason) override { wsRequestClose(s_, code, reason); }
    bool isOpen() const override { return s_->open.load() && !s_->done.load(); }

private:
    std::shared_ptr<WsSession> s_;
};

} // namespace

std::shared_ptr<WsSession> wsMakeSession(const WsConfig& cfg) {
    auto s = std::make_shared<WsSession>();
    s->cfg = cfg;
    return s;
}

std::shared_ptr<IStreamChannel> wsMakeChannel(const std::shared_ptr<WsSession>& session) {
    return std::make_shared<WsChannel>(session);
}

void wsRequestClose(const std::shared_ptr<WsSession>& session, int code, const std::string& reason) {
    if (!session) return;
    std::lock_guard<std::mutex> lk(session->mu);
    session->wantClose = true;
    session->closeCode = code;
    session->closeReason = reason;
    session->cv.notify_all();
}

namespace {

struct WsResult {
    StreamStatus status = StreamStatus::Ok;
    int code = 1000;
    std::string msg;
    std::uint64_t seq = 0;
    std::uint64_t bytes = 0;
};

// Output wiring for the pump. Exactly one of {sink, hooks} is set: `sink` = default frame-log stream
// (openSession); `hooks` = a protocol interprets frames itself (graphql-transport-ws). `cancel` (optional)
// lets openStream-based callers stop the pump with a graceful close.
struct WsPumpIO {
    IStreamSink* sink = nullptr;
    const WsFrameHooks* hooks = nullptr;
    CancelToken* cancel = nullptr;
};

// Drives the post-handshake WebSocket pump loop (SPEC §5): one tick = recv inbound, honor a close
// request, drain the outbound queue, keepalive-ping, idle-check. Owns the per-session loop state so the
// six phases stay as small methods. The CURL handle is BORROWED (freed by the caller's RAII guards).
class WsPump {
public:
    WsPump(CURL* curl, curl_socket_t sock, const std::shared_ptr<WsSession>& session,
           long long t0, const WsPumpIO& io)
        : curl_(curl), sock_(sock), session_(session), io_(io), cfg_(session->cfg), t0_(t0),
          lastPing_(nowMs()), lastActivity_(nowMs()) {}

    WsResult run() {
        while (running_) {
            waitReadable();         // 1) block up to one tick on readability (also paces send/ping)
            drainInbound();         // 2) recv + reassemble frames; sets the result on close/error
            if (!running_) break;
            if (handleCloseRequest()) break;   // 3) UI Disconnect/cancel -> send CLOSE, stop
            drainOutbound();        // 4) flush queued outbound frames
            if (!running_) break;
            keepalive();            // 5) periodic PING (libcurl never pings on its own, §3.1)
            if (idleExpired()) break;          // 6) no activity for too long -> dead connection
        }
        result_.seq = seq_;
        result_.bytes = bytes_;
        return result_;
    }

private:
    long long offsetMs() const { return nowMs() - t0_; }

    void fail(int code, std::string msg) {
        result_.status = StreamStatus::Error;
        result_.code = code;
        result_.msg = std::move(msg);
        running_ = false;
    }

    bool cancelRequested() const { return io_.cancel && io_.cancel->cancelled(); }

    // Deliver one data frame. Protocol mode: hand raw INBOUND text to the protocol (outbound not logged).
    // Default mode: build a neutral frame-log element and emit it on the sink (in + out).
    void emitData(StreamDirection dir, unsigned flags, const std::uint8_t* data, std::size_t n) {
        if (io_.hooks) {
            if (dir == StreamDirection::Inbound && io_.hooks->onText)
                io_.hooks->onText(std::string(reinterpret_cast<const char*>(data), n));
            return;
        }
        if (!io_.sink) return;
        bool binary = (flags & CURLWS_BINARY) != 0;
        std::string payload = frameEnvelope(dir, binary, offsetMs(), data, n);
        StreamEvent ev;
        ev.seq = seq_;
        ev.direction = dir;
        ev.frameType = binary ? StreamFrameType::Binary : StreamFrameType::Text;
        ev.kind = binary ? StreamPayloadKind::Binary : StreamPayloadKind::Text;
        ev.payload = payload;
        ev.offsetMs = offsetMs();
        io_.sink->onStreamEvent(ev);
        ++seq_;
        bytes_ += payload.size();
    }

    void waitReadable() {
        if (sock_ == CURL_SOCKET_BAD) return;
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(sock_, &rfd);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;   // 50ms tick
        select((int)sock_ + 1, &rfd, nullptr, nullptr, &tv);
    }

    // A received CLOSE frame -> record the peer's code/reason and stop.
    void onInboundClose(const char* buf, std::size_t nread) {
        int code = 1005;
        std::string reason;
        if (nread >= 2) {
            code = ((unsigned char)buf[0] << 8) | (unsigned char)buf[1];
            reason.assign(buf + 2, nread - 2);
        }
        result_.status = (code == 1000) ? StreamStatus::Ok : StreamStatus::Error;
        result_.code = code;
        result_.msg = reason;
        running_ = false;
    }

    // Accumulate a data frame (TEXT/BINARY/CONT) until complete, then emit it. Returns false on overflow.
    bool accumulateFrame(const char* buf, std::size_t nread, unsigned f, const struct curl_ws_frame* m) {
        if (frameBuf_.empty()) frameFlags_ = f;
        frameBuf_.append(buf, nread);
        if (frameBuf_.size() > cfg_.maxFrameBytes) {
            fail(1009, "inbound frame exceeded WS_MAX_FRAME_BYTES");   // 1009 = message too big
            return false;
        }
        if (m && m->bytesleft == 0) {   // frame fully received -> emit one log element
            emitData(StreamDirection::Inbound, frameFlags_,
                     reinterpret_cast<const std::uint8_t*>(frameBuf_.data()), frameBuf_.size());
            frameBuf_.clear();
            frameFlags_ = 0;
        }
        return true;
    }

    // Drain all available inbound data (non-blocking; CONNECT_ONLY sockets return CURLE_AGAIN).
    void drainInbound() {
        while (running_) {
            char buf[16384];
            std::size_t nread = 0;
            const struct curl_ws_frame* m = nullptr;
            CURLcode r = curl_ws_recv(curl_, buf, sizeof(buf), &nread, &m);
            if (r == CURLE_AGAIN) break;
            if (r != CURLE_OK) {   // closed/broken without a CLOSE frame -> abnormal (1006)
                fail(1006, std::string("connection lost: ") + curl_easy_strerror(r));
                break;
            }
            lastActivity_ = nowMs();
            unsigned f = m ? (unsigned)m->flags : 0;
            if (f & CURLWS_CLOSE) { onInboundClose(buf, nread); break; }
            if (f & (CURLWS_PING | CURLWS_PONG)) continue;   // libcurl auto-PONGs; count as activity only
            if (!accumulateFrame(buf, nread, f, m)) break;
        }
    }

    // Graceful close requested (UI Disconnect / wantClose / cancel) -> send CLOSE. Returns true if we stop.
    bool handleCloseRequest() {
        bool doClose = false;
        int reqCode = 1000;
        std::string reqReason;
        {
            std::lock_guard<std::mutex> lk(session_->mu);
            if (session_->wantClose) { doClose = true; reqCode = session_->closeCode; reqReason = session_->closeReason; }
        }
        bool cancelled = cancelRequested();
        if (!doClose && cancelled) doClose = true;   // openStream cancel -> graceful close
        if (!doClose) return false;
        std::vector<std::uint8_t> payload;
        payload.push_back((std::uint8_t)((reqCode >> 8) & 0xFF));
        payload.push_back((std::uint8_t)(reqCode & 0xFF));
        payload.insert(payload.end(), reqReason.begin(), reqReason.end());
        std::size_t sent = 0;
        curl_ws_send(curl_, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);
        result_.status = cancelled ? StreamStatus::Cancelled : StreamStatus::Ok;
        result_.code = reqCode;
        result_.msg = reqReason;
        return true;
    }

    // Drain the outbound queue (one frame at a time; CURLE_AGAIN -> retry next tick).
    void drainOutbound() {
        while (running_) {
            OutFrame f;
            {
                std::lock_guard<std::mutex> lk(session_->mu);
                if (session_->out.empty()) break;
                f = session_->out.front();
            }
            std::size_t sent = 0;
            CURLcode sr = curl_ws_send(curl_, f.data.data(), f.data.size(), &sent, 0, f.flags);
            if (sr == CURLE_AGAIN) break;   // socket not writable now -> keep queued, try later
            {
                std::lock_guard<std::mutex> lk(session_->mu);
                if (!session_->out.empty()) {
                    session_->outBytes -= session_->out.front().data.size();
                    session_->out.pop_front();
                }
            }
            if (sr != CURLE_OK) { fail(1006, std::string("send failed: ") + curl_easy_strerror(sr)); break; }
            lastActivity_ = nowMs();
            if (f.flags & (CURLWS_TEXT | CURLWS_BINARY))   // log only data frames (not ping/close)
                emitData(StreamDirection::Outbound, f.flags, f.data.data(), f.data.size());
        }
    }

    void keepalive() {
        if (cfg_.pingIntervalMs > 0 && nowMs() - lastPing_ >= cfg_.pingIntervalMs) {
            std::size_t sent = 0;
            curl_ws_send(curl_, "", 0, &sent, 0, CURLWS_PING);
            lastPing_ = nowMs();
        }
    }

    bool idleExpired() {
        if (cfg_.idleTimeoutMs > 0 && nowMs() - lastActivity_ >= cfg_.idleTimeoutMs) {
            result_.status = StreamStatus::Timeout;
            result_.code = 1006;
            result_.msg = "idle timeout";
            return true;
        }
        return false;
    }

    CURL* curl_;
    curl_socket_t sock_;
    std::shared_ptr<WsSession> session_;
    WsPumpIO io_;
    const WsConfig& cfg_;
    long long t0_;
    long long lastPing_;
    long long lastActivity_;
    std::uint64_t seq_ = 0;
    std::uint64_t bytes_ = 0;
    std::string frameBuf_;       // inbound reassembly (per frame, until bytesleft==0)
    unsigned frameFlags_ = 0;
    bool running_ = true;
    WsResult result_;
};

// Build the handshake header list (auth + custom headers + Sec-WebSocket-Protocol). Caller owns the list.
struct curl_slist* buildWsHandshakeHeaders(const WsRequest& w) {
    struct curl_slist* hdrs = nullptr;
    std::vector<KeyValue> headers = w.headers;
    applyAuthHeaders(w.auth, headers);   // bearer/basic/apikey -> handshake header (no per-message headers)
    for (const auto& kv : headers)
        if (kv.enabled && !kv.key.empty())
            hdrs = curl_slist_append(hdrs, (kv.key + ": " + kv.value).c_str());
    if (!w.subprotocols.empty()) {
        std::string sp = "Sec-WebSocket-Protocol: ";
        for (std::size_t i = 0; i < w.subprotocols.size(); ++i) sp += (i ? ", " : "") + w.subprotocols[i];
        hdrs = curl_slist_append(hdrs, sp.c_str());
    }
    return hdrs;
}

// Apply the WebSocket handshake options to the easy handle (CONNECT_ONLY=2 -> handshake then hand back).
void configureWsHandshake(CURL* curl, const WsRequest& w, const WsConfig& cfg, struct curl_slist* hdrs) {
    curl_easy_setopt(curl, CURLOPT_URL, w.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)cfg.connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);   // small frames not held by Nagle (perf §11)
}

// Emit the terminal StreamEnd and mark the session closed/done (§3 close contract).
void wsEmitClose(IStreamSink& sink, const std::shared_ptr<WsSession>& session, const WsResult& r,
                 long long elapsedMs) {
    StreamEnd end;
    end.status = r.status;
    end.statusCode = r.code;
    end.statusMessage = r.msg;
    end.totalEvents = r.seq;
    end.totalBytes = r.bytes;
    end.elapsedMs = elapsedMs;
    sink.onStreamClose(end);
    session->open.store(false);
    session->done.store(true);
}

} // namespace

void wsRun(const ResolvedRequest& req, IStreamSink& sink,
           const std::shared_ptr<WsSession>& session, const std::string& sessionId) {
    const WsRequest& w = req.model.ws;
    const WsConfig& cfg = session->cfg;
    const long long t0 = nowMs();
    auto offsetMs = [&] { return nowMs() - t0; };

    StreamMeta meta;
    meta.streamId = sessionId;
    meta.transport = StreamTransport::WebSocket;
    meta.startedAtEpochMs = nowEpochMs();

    if (!curlHasWebSocket()) {
        sink.onStreamOpen(meta);
        wsEmitClose(sink, session, WsResult{StreamStatus::Error, 0,
                    "libcurl was built without WebSocket support (need curl[websockets], libcurl >= 8.11)",
                    0, 0}, offsetMs());
        return;
    }

    // RAII (L13): the easy handle + header slist free themselves on EVERY return path, so a future early
    // return between here and the end can't leak.
    auto curlDel = [](CURL* c) { if (c) curl_easy_cleanup(c); };
    std::unique_ptr<CURL, decltype(curlDel)> curlGuard(curl_easy_init(), curlDel);
    CURL* curl = curlGuard.get();
    if (!curl) {
        sink.onStreamOpen(meta);
        wsEmitClose(sink, session, WsResult{StreamStatus::Error, 0, "curl init failed", 0, 0}, offsetMs());
        return;
    }

    auto slistDel = [](struct curl_slist* s) { if (s) curl_slist_free_all(s); };
    struct curl_slist* hdrs = buildWsHandshakeHeaders(w);
    // L13: guard owns the list now (curl_easy_setopt only borrows the pointer).
    std::unique_ptr<struct curl_slist, decltype(slistDel)> hdrsGuard(hdrs, slistDel);
    configureWsHandshake(curl, w, cfg, hdrs);

    CURLcode rc = curl_easy_perform(curl);                        // handshake
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    sink.onStreamOpen(meta);   // §3-contract: always one open before any close
    if (rc != CURLE_OK) {
        std::string msg = std::string("WebSocket handshake failed: ") + curl_easy_strerror(rc);
        if (httpCode) msg += " (HTTP " + std::to_string(httpCode) + ")";
        // curl + hdrs freed by their RAII guards (L13).
        wsEmitClose(sink, session, WsResult{StreamStatus::Error, (int)httpCode, msg, 0, 0}, offsetMs());
        return;
    }
    session->open.store(true);

    // onOpenSend: queue subscribe-style messages to fire right after open (§1/§5).
    for (const auto& m : w.onOpenSend)
        enqueue(session, OutFrame{std::vector<std::uint8_t>(m.begin(), m.end()), CURLWS_TEXT, 0});

    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);

    WsPumpIO io;
    io.sink = &sink;
    WsResult res = WsPump(curl, sock, session, t0, io).run();
    wsEmitClose(sink, session, res, offsetMs());   // curl + hdrs freed by their RAII guards on return (L13)
}

void wsRunProtocol(const ResolvedRequest& req, const WsFrameHooks& hooks,
                   const std::shared_ptr<WsSession>& session, const std::string& sessionId,
                   const std::shared_ptr<CancelToken>& cancel) {
    (void)sessionId;
    const WsRequest& w = req.model.ws;
    const WsConfig& cfg = session->cfg;
    const long long t0 = nowMs();
    // On any terminal outcome, the protocol owns the §3 open/close contract via hooks.onClose.
    auto finish = [&](StreamStatus st, int code, const std::string& msg) {
        if (hooks.onClose) hooks.onClose(st, code, msg);
        session->open.store(false);
        session->done.store(true);
    };

    if (!curlHasWebSocket()) {
        finish(StreamStatus::Error, 0,
               "libcurl was built without WebSocket support (need curl[websockets], libcurl >= 8.11)");
        return;
    }
    auto curlDel = [](CURL* c) { if (c) curl_easy_cleanup(c); };
    std::unique_ptr<CURL, decltype(curlDel)> curlGuard(curl_easy_init(), curlDel);
    CURL* curl = curlGuard.get();
    if (!curl) { finish(StreamStatus::Error, 0, "curl init failed"); return; }

    auto slistDel = [](struct curl_slist* s) { if (s) curl_slist_free_all(s); };
    struct curl_slist* hdrs = buildWsHandshakeHeaders(w);   // auth + custom + Sec-WebSocket-Protocol
    std::unique_ptr<struct curl_slist, decltype(slistDel)> hdrsGuard(hdrs, slistDel);
    configureWsHandshake(curl, w, cfg, hdrs);

    CURLcode rc = curl_easy_perform(curl);   // handshake
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (rc != CURLE_OK) {
        std::string msg = std::string("WebSocket handshake failed: ") + curl_easy_strerror(rc);
        if (httpCode) msg += " (HTTP " + std::to_string(httpCode) + ")";
        finish(StreamStatus::Error, (int)httpCode, msg);
        return;
    }
    session->open.store(true);
    if (hooks.onOpen) hooks.onOpen();   // protocol sends connection_init (enqueued; the pump drains it)

    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);

    WsPumpIO io;
    io.hooks = &hooks;
    io.cancel = cancel.get();
    WsResult res = WsPump(curl, sock, session, t0, io).run();
    finish(res.status, res.code, res.msg);   // curl + hdrs freed by their RAII guards on return (L13)
}

} // namespace core
