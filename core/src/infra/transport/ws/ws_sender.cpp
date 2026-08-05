// ws_sender.cpp — WebSocket transport over libcurl (SPEC_websocket §3/§5/§10).
#include "infra/transport/ws/ws_sender.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>

#include <nlohmann/json.hpp>

#include "infra/transport/shared/socket_abort.hpp" // connect phase: no callback runs, shutdown() the fd

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace core {
namespace d = core::domain;

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
    std::deque<OutFrame> out;
    std::uint64_t outBytes = 0;
    bool wantClose = false;
    int closeCode = 1000;
    std::string closeReason;
    std::atomic<bool> open{false};
    std::atomic<bool> done{false};
    // Handshake escape hatch: while curl_easy_perform is still connecting nothing polls wantClose, so a
    // close/cancel request shuts the socket down instead. Owned here so any thread can reach it.
    std::shared_ptr<SocketAbort> sockets = std::make_shared<SocketAbort>();
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
        auto j = nlohmann::json::parse(data, data + n, nullptr, false);
        if (!j.is_discarded()) env["data"] = std::move(j);   // embed parsed JSON when possible
        else env["data"] = std::string(reinterpret_cast<const char*>(data), n);   // else raw string
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
    {
        std::lock_guard<std::mutex> lk(session->mu);
        session->wantClose = true;
        session->closeCode = code;
        session->closeReason = reason;
    }
    // Not open yet == still in the handshake, where the pump's checks don't run. Pull the socket down.
    // Once open, leave it alone: the pump owes the peer a graceful CLOSE frame.
    if (!session->open.load()) session->sockets->abort();
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
        ev.payload = std::move(payload);
        ev.offsetMs = offsetMs();
        io_.sink->onStreamEvent(ev);
        ++seq_;
        bytes_ += ev.payload.size();
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

    // After our CLOSE went out, wait (bounded by closeTimeoutMs) for the peer's CLOSE echo; non-CLOSE
    // frames received meanwhile are discarded.
    void awaitCloseHandshake() {
        const long long deadline = nowMs() + cfg_.closeTimeoutMs;
        while (cfg_.closeTimeoutMs > 0 && nowMs() < deadline) {
            waitReadable();
            char buf[16384];
            std::size_t nread = 0;
            const struct curl_ws_frame* m = nullptr;
            CURLcode r = curl_ws_recv(curl_, buf, sizeof(buf), &nread, &m);
            if (r == CURLE_AGAIN) continue;
            if (r != CURLE_OK) return;                       // connection gone -> nothing to wait for
            if (m && (m->flags & CURLWS_CLOSE)) return;      // peer echoed CLOSE -> handshake complete
        }
    }

    // Graceful close requested (UI Disconnect / wantClose / cancel) -> send CLOSE, then wait for the
    // peer's echo. Returns true if we stop (false while the socket is not writable yet).
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
        CURLcode sr = curl_ws_send(curl_, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);
        if (sr == CURLE_AGAIN) return false;   // socket not writable now -> retry next tick
        if (sr == CURLE_OK) awaitCloseHandshake();
        result_.status = cancelled ? StreamStatus::Cancelled : StreamStatus::Ok;
        result_.code = reqCode;
        result_.msg = reqReason;
        return true;
    }

    // Drain the outbound queue (one frame at a time; CURLE_AGAIN -> retry next tick). The front element
    // is used through a stable pointer instead of a copy: only this thread pops, and deque::push_back
    // never invalidates references to existing elements.
    void drainOutbound() {
        while (running_) {
            const OutFrame* f = nullptr;
            {
                std::lock_guard<std::mutex> lk(session_->mu);
                if (session_->out.empty()) break;
                f = &session_->out.front();
            }
            std::size_t sent = 0;
            CURLcode sr = curl_ws_send(curl_, f->data.data(), f->data.size(), &sent, 0, f->flags);
            if (sr == CURLE_AGAIN) break;   // socket not writable now -> keep queued, try later
            if (sr != CURLE_OK) { fail(1006, std::string("send failed: ") + curl_easy_strerror(sr)); break; }
            lastActivity_ = nowMs();
            if (f->flags & (CURLWS_TEXT | CURLWS_BINARY))   // log only data frames (not ping/close)
                emitData(StreamDirection::Outbound, f->flags, f->data.data(), f->data.size());
            {
                std::lock_guard<std::mutex> lk(session_->mu);
                session_->outBytes -= f->data.size();
                session_->out.pop_front();
            }
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

// Apply a domain Auth to the handshake header list (bearer/basic -> Authorization header).
void applyWsAuth(const d::Auth& auth, std::vector<std::pair<std::string, std::string>>& hdrs) {
    auth.match([&](auto&& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, d::AuthBearer>) {
            hdrs.push_back({"Authorization", "Bearer " + a.token});
        } else if constexpr (std::is_same_v<T, d::AuthBasic>) {
            std::string creds = a.username + ":" + a.password;
            hdrs.push_back({"Authorization",
                            "Basic " + base64(reinterpret_cast<const std::uint8_t*>(creds.data()),
                                              creds.size())});
        }
    });
}

// Build the handshake header list (auth + custom headers + Sec-WebSocket-Protocol). Caller owns the list.
struct curl_slist* buildWsHandshakeHeaders(const d::WebSocketRequest& w) {
    struct curl_slist* hdrs = nullptr;
    std::vector<std::pair<std::string, std::string>> headers;
    for (const auto& h : w.headers().items())
        if (h.enabled() && !h.name().empty()) headers.push_back({h.name(), h.value()});
    applyWsAuth(w.auth(), headers);   // bearer/basic -> handshake header (no per-message headers)
    for (const auto& kv : headers)
        hdrs = curl_slist_append(hdrs, (kv.first + ": " + kv.second).c_str());
    const auto& subs = w.subprotocols();
    if (!subs.empty()) {
        std::string sp = "Sec-WebSocket-Protocol: ";
        for (std::size_t i = 0; i < subs.size(); ++i) sp += (i ? ", " : "") + subs[i];
        hdrs = curl_slist_append(hdrs, sp.c_str());
    }
    return hdrs;
}

// Abort state for a handshake still in curl_easy_perform. The pump's cancel checks only run AFTER the
// handshake returns, so without this a peer that accepts TCP then never answers the upgrade parks the
// thread and Cancel does nothing.
struct HandshakeAbort {
    WsSession* session = nullptr;
    CancelToken* cancel = nullptr;
    bool tripped() {
        if (cancel && cancel->cancelled()) return true;
        if (!session) return false;
        std::lock_guard<std::mutex> lk(session->mu);
        return session->wantClose;
    }
};

// Non-zero -> curl_easy_perform returns CURLE_ABORTED_BY_CALLBACK.
int handshakeAbortCb(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* a = static_cast<HandshakeAbort*>(p);
    return (a && a->tripped()) ? 1 : 0;
}

// Apply the WebSocket handshake options to the easy handle (CONNECT_ONLY=2 -> handshake then hand back).
void configureWsHandshake(CURL* curl, const d::WebSocketRequest& w, const WsConfig& cfg,
                          struct curl_slist* hdrs, HandshakeAbort* abort, SocketAbort* sockets) {
    curl_easy_setopt(curl, CURLOPT_URL, w.url().raw().c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)cfg.connectTimeoutMs);
    // Bound the WHOLE handshake, not just the TCP/TLS connect: a peer that stalls after the connect used
    // to hold the thread forever (CONNECT_ONLY=2 returns as soon as the upgrade completes anyway).
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)cfg.connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);   // small frames not held by Nagle (perf §11)
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, handshakeAbortCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, abort);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    SocketAbort::install(curl, sockets);
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

// One handshake -> pump run, shared by wsRun and wsRunProtocol: owns the curl/slist RAII guards (L13),
// performs the handshake, sets session->open, runs WsPump. On any pre-pump failure (no ws support, init,
// handshake) it calls `onFail` and returns nullopt; otherwise `onOpen` fires once right after the
// handshake succeeds (before the pump starts) and the pump result is returned.
std::optional<WsResult> wsHandshakeAndPump(
        const d::WebSocketRequest& w, const std::shared_ptr<WsSession>& session, long long t0,
        const WsPumpIO& io, const std::function<void(StreamStatus, int, const std::string&)>& onFail,
        const std::function<void()>& onOpen) {
    if (!curlHasWebSocket()) {
        onFail(StreamStatus::Error, 0,
               "libcurl was built without WebSocket support (need curl[websockets], libcurl >= 8.11)");
        return std::nullopt;
    }

    // RAII (L13): the easy handle + header slist free themselves on EVERY return path, so a future early
    // return between here and the end can't leak.
    auto curlDel = [](CURL* c) { if (c) curl_easy_cleanup(c); };
    std::unique_ptr<CURL, decltype(curlDel)> curlGuard(curl_easy_init(), curlDel);
    CURL* curl = curlGuard.get();
    if (!curl) {
        onFail(StreamStatus::Error, 0, "curl init failed");
        return std::nullopt;
    }

    auto slistDel = [](struct curl_slist* s) { if (s) curl_slist_free_all(s); };
    struct curl_slist* hdrs = buildWsHandshakeHeaders(w);   // auth + custom + Sec-WebSocket-Protocol
    // L13: guard owns the list now (curl_easy_setopt only borrows the pointer).
    std::unique_ptr<struct curl_slist, decltype(slistDel)> hdrsGuard(hdrs, slistDel);
    HandshakeAbort abort{session.get(), io.cancel};   // outlives curl_easy_perform below
    configureWsHandshake(curl, w, session->cfg, hdrs, &abort, session->sockets.get());
    if (abort.tripped()) session->sockets->abort();   // cancelled before we even got here

    CURLcode rc = curl_easy_perform(curl);                        // handshake
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (rc == CURLE_ABORTED_BY_CALLBACK) {   // Cancel/Disconnect landed mid-handshake
        onFail(StreamStatus::Cancelled, (int)httpCode, "cancelled");
        return std::nullopt;
    }
    if (rc != CURLE_OK) {
        std::string msg = std::string("WebSocket handshake failed: ") + curl_easy_strerror(rc);
        if (httpCode) msg += " (HTTP " + std::to_string(httpCode) + ")";
        onFail(StreamStatus::Error, (int)httpCode, msg);
        return std::nullopt;
    }
    session->open.store(true);
    if (onOpen) onOpen();

    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);

    return WsPump(curl, sock, session, t0, io).run();
    // curl + hdrs freed by their RAII guards on return (L13)
}

} // namespace

void wsRun(const d::WebSocketRequest& w, IStreamSink& sink,
           const std::shared_ptr<WsSession>& session, const std::string& sessionId) {
    const long long t0 = nowMs();
    auto offsetMs = [&] { return nowMs() - t0; };

    StreamMeta meta;
    meta.streamId = sessionId;
    meta.transport = StreamTransport::WebSocket;
    meta.startedAtEpochMs = nowEpochMs();

    WsPumpIO io;
    io.sink = &sink;
    auto res = wsHandshakeAndPump(
        w, session, t0, io,
        [&](StreamStatus st, int code, const std::string& msg) {
            sink.onStreamOpen(meta);   // §3-contract: always one open before any close
            wsEmitClose(sink, session, WsResult{st, code, msg, 0, 0}, offsetMs());
        },
        [&] {
            sink.onStreamOpen(meta);   // §3-contract: always one open before any close
            // onOpenSend: queue subscribe-style messages to fire right after open (§1/§5).
            for (const auto& m : w.onOpenSend())
                enqueue(session,
                        OutFrame{std::vector<std::uint8_t>(m.payload.begin(), m.payload.end()),
                                 static_cast<unsigned>((m.kind == d::WsSendKind::Binary) ? CURLWS_BINARY
                                                                                         : CURLWS_TEXT),
                                 0});
        });
    if (res) wsEmitClose(sink, session, *res, offsetMs());
}

void wsRunProtocol(const d::WebSocketRequest& w, const WsFrameHooks& hooks,
                   const std::shared_ptr<WsSession>& session, const std::string& sessionId,
                   const std::shared_ptr<CancelToken>& cancel) {
    (void)sessionId;
    const long long t0 = nowMs();
    // On any terminal outcome, the protocol owns the §3 open/close contract via hooks.onClose.
    auto finish = [&](StreamStatus st, int code, const std::string& msg) {
        if (hooks.onClose) hooks.onClose(st, code, msg);
        session->open.store(false);
        session->done.store(true);
    };

    WsPumpIO io;
    io.hooks = &hooks;
    io.cancel = cancel.get();
    auto res = wsHandshakeAndPump(
        w, session, t0, io, finish,
        [&] { if (hooks.onOpen) hooks.onOpen(); });   // protocol sends connection_init (enqueued; the pump drains it)
    if (res) finish(res->status, res->code, res->msg);
}

} // namespace core

// ---- WsSenderAdapter (domain IRequestSender) — merged from ws_sender_adapter.cpp (RESTRUCTURE_PLAN S3) ----
#include <exception>
#include <variant>

namespace core::infra {
namespace d = core::domain;

namespace {
// Legacy inbound IStreamSink -> domain ResponseEvents. Lives on the stack for one wsRun() call.
struct WsInboundTranslator final : core::IStreamSink {
  d::IResponseSink *sink;

  void onStreamOpen(const core::StreamMeta &m) override {
    std::vector<d::ResponseHeader> hs;
    for (const auto &kv : m.leading) hs.push_back({kv.key, kv.value});
    if (!hs.empty()) sink->emit(d::ResponseEvent(d::EvMetadata{std::move(hs)}));
  }
  void onStreamEvent(const core::StreamEvent &ev) override {
    auto kind = (ev.kind == core::StreamPayloadKind::Binary) ? d::WsSendKind::Binary : d::WsSendKind::Text;
    sink->emit(d::ResponseEvent(d::EvMessage{kind, ev.payload, ev.seq}));
  }
  void onStreamClose(const core::StreamEnd &end) override {
    if (end.status == core::StreamStatus::Error || end.status == core::StreamStatus::Timeout) {
      d::ErrorKind k = (end.status == core::StreamStatus::Timeout) ? d::ErrorKind::Timeout
                                                                   : d::ErrorKind::Protocol;
      sink->emit(d::ResponseEvent(d::EvFailed{{k, end.statusMessage, end.statusCode}}));
    } else {
      sink->emit(d::ResponseEvent(d::EvClosed{end.statusCode, end.statusMessage}));
    }
  }
};
} // namespace

d::Status WsSenderAdapter::execute(const d::RequestModel &resolved, d::IResponseSink &sink,
                                   const d::ICancellationToken &cancel) {
  // The model reaching here is already {{var}}-resolved (CoreApiClient::send -> DomainVariableResolver).
  if (resolved.type() != d::RequestType::WebSocket) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Unsupported, "not a websocket request", {}}}));
    return d::ok();
  }
  const d::WebSocketRequest &w = std::get<d::WebSocketRequest>(resolved.payload());

  // .env base + per-request RequestConfig (TLS verify + idle timeout) — mirrors Engine::buildWsConfig.
  core::WsConfig cfg = base_;
  cfg.verifyTls = resolved.config().tlsEnabledDefault;
  if (resolved.config().timeout.millis() > 0)
    cfg.idleTimeoutMs = static_cast<int>(resolved.config().timeout.millis());

  const std::string sid = "ws-" + std::to_string(nextId_++);

  auto session = wsMakeSession(cfg);
  auto channel = wsMakeChannel(session);
  {
    std::lock_guard<std::mutex> lk(mu_);
    channel_ = channel;
  }
  // Cancel from any thread -> close request the pump AND a stuck handshake both honor. Fires right away
  // when the caller is already cancelled, so this covers the pre-flight case too.
  cancel.onCancel([session] { wsRequestClose(session, 1000, "cancelled"); });

  WsInboundTranslator translator;
  translator.sink = &sink;
  try {
    // Runs the recv/send pump on THIS thread; emits onStreamOpen/Event/Close on the translator, then returns
    // after the close handshake (keeps the saga's pool worker parked for the whole session).
    wsRun(w, translator, session, sid);
  } catch (const std::exception &e) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Internal, e.what(), {}}}));
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    channel_.reset();
  }
  return d::ok();
}

d::Status WsSenderAdapter::push(d::WsMessage m) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!channel_) return d::Status::fail({d::ErrorCode::Unsupported, "no open ws session"});
  bool ok = (m.kind == d::WsSendKind::Binary)
                ? channel_->sendBinary(std::vector<std::uint8_t>(m.payload.begin(), m.payload.end()))
                : channel_->sendText(m.payload);
  return ok ? d::ok() : d::Status::fail({d::ErrorCode::Internal, "ws send queue full"});
}

d::Status WsSenderAdapter::close(int code, std::string reason) {
  std::lock_guard<std::mutex> lk(mu_);
  if (channel_) channel_->close(code, reason);
  return d::ok();
}

} // namespace core::infra
