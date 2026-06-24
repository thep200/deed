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

void wsRun(const ResolvedRequest& req, IStreamSink& sink,
           const std::shared_ptr<WsSession>& session, const std::string& sessionId) {
    const WsRequest& w = req.model.ws;
    const WsConfig& cfg = session->cfg;
    const long long t0 = nowMs();
    auto offsetMs = [&] { return nowMs() - t0; };
    std::uint64_t seq = 0, bytes = 0;

    StreamMeta meta;
    meta.streamId = sessionId;
    meta.transport = StreamTransport::WebSocket;
    meta.startedAtEpochMs = nowEpochMs();

    auto closeWith = [&](StreamStatus st, int code, const std::string& msg) {
        StreamEnd end;
        end.status = st;
        end.statusCode = code;
        end.statusMessage = msg;
        end.totalEvents = seq;
        end.totalBytes = bytes;
        end.elapsedMs = offsetMs();
        sink.onStreamClose(end);
        session->open.store(false);
        session->done.store(true);
    };

    if (!curlHasWebSocket()) {
        sink.onStreamOpen(meta);
        closeWith(StreamStatus::Error, 0,
                  "libcurl was built without WebSocket support (need curl[websockets], libcurl >= 8.11)");
        return;
    }

    // RAII (L13): the easy handle + header slist free themselves on EVERY return path, so a future early
    // return between here and the end can't leak.
    auto curlDel = [](CURL* c) { if (c) curl_easy_cleanup(c); };
    std::unique_ptr<CURL, decltype(curlDel)> curlGuard(curl_easy_init(), curlDel);
    CURL* curl = curlGuard.get();
    if (!curl) { sink.onStreamOpen(meta); closeWith(StreamStatus::Error, 0, "curl init failed"); return; }

    auto slistDel = [](struct curl_slist* s) { if (s) curl_slist_free_all(s); };
    std::unique_ptr<struct curl_slist, decltype(slistDel)> hdrsGuard(nullptr, slistDel);
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
    hdrsGuard.reset(hdrs);   // L13: guard owns the list now (curl_easy_setopt only borrows the pointer)

    curl_easy_setopt(curl, CURLOPT_URL, w.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);             // WebSocket: handshake then hand back control
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)cfg.connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);             // small frames not held by Nagle (perf §11)

    CURLcode rc = curl_easy_perform(curl);                        // handshake
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    sink.onStreamOpen(meta);   // §3-contract: always one open before any close
    if (rc != CURLE_OK) {
        std::string msg = std::string("WebSocket handshake failed: ") + curl_easy_strerror(rc);
        if (httpCode) msg += " (HTTP " + std::to_string(httpCode) + ")";
        closeWith(StreamStatus::Error, (int)httpCode, msg);   // curl + hdrs freed by their RAII guards (L13)
        return;
    }
    session->open.store(true);

    // onOpenSend: queue subscribe-style messages to fire right after open (§1/§5).
    for (const auto& m : w.onOpenSend)
        enqueue(session, OutFrame{std::vector<std::uint8_t>(m.begin(), m.end()), CURLWS_TEXT, 0});

    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);

    // Result state filled by the pump.
    StreamStatus endStatus = StreamStatus::Ok;
    int endCode = 1000;
    std::string endMsg;

    std::string frameBuf;       // inbound reassembly (per frame, until bytesleft==0)
    unsigned frameFlags = 0;
    long long lastPing = nowMs(), lastActivity = nowMs();
    bool sentClose = false;
    bool running = true;

    while (running) {
        // 1) Wait briefly for readability (also our tick for send/ping). select gives a ~tick even idle.
        if (sock != CURL_SOCKET_BAD) {
            fd_set rfd;
            FD_ZERO(&rfd);
            FD_SET(sock, &rfd);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 50 * 1000;   // 50ms tick
            select((int)sock + 1, &rfd, nullptr, nullptr, &tv);
        }

        // 2) Drain all available inbound data (non-blocking; CONNECT_ONLY sockets return CURLE_AGAIN).
        while (running) {
            char buf[16384];
            std::size_t nread = 0;
            const struct curl_ws_frame* m = nullptr;
            CURLcode r = curl_ws_recv(curl, buf, sizeof(buf), &nread, &m);
            if (r == CURLE_AGAIN) break;
            if (r != CURLE_OK) {
                // Connection closed/broken without a CLOSE frame -> abnormal (1006).
                endStatus = StreamStatus::Error;
                endCode = 1006;
                endMsg = std::string("connection lost: ") + curl_easy_strerror(r);
                running = false;
                break;
            }
            lastActivity = nowMs();
            unsigned f = m ? (unsigned)m->flags : 0;

            if (f & CURLWS_CLOSE) {
                int code = 1005;
                std::string reason;
                if (nread >= 2) {
                    code = ((unsigned char)buf[0] << 8) | (unsigned char)buf[1];
                    reason.assign(buf + 2, nread - 2);
                }
                endStatus = (code == 1000) ? StreamStatus::Ok : StreamStatus::Error;
                endCode = code;
                endMsg = reason;
                running = false;
                break;
            }
            if (f & (CURLWS_PING | CURLWS_PONG)) {
                continue;   // libcurl auto-PONGs server PINGs; we just count as activity (not shown §5)
            }

            // Data frame (TEXT/BINARY/CONT): accumulate until the frame completes.
            if (frameBuf.empty()) frameFlags = f;
            frameBuf.append(buf, nread);
            if (frameBuf.size() > cfg.maxFrameBytes) {
                endStatus = StreamStatus::Error;
                endCode = 1009;   // message too big
                endMsg = "inbound frame exceeded WS_MAX_FRAME_BYTES";
                running = false;
                break;
            }
            if (m && m->bytesleft == 0) {   // frame fully received -> emit one log element
                bool binary = (frameFlags & CURLWS_BINARY) != 0;
                std::string payload = frameEnvelope(StreamDirection::Inbound, binary, offsetMs(),
                                                    reinterpret_cast<const std::uint8_t*>(frameBuf.data()),
                                                    frameBuf.size());
                StreamEvent ev;
                ev.seq = seq;
                ev.direction = StreamDirection::Inbound;
                ev.frameType = binary ? StreamFrameType::Binary : StreamFrameType::Text;
                ev.kind = binary ? StreamPayloadKind::Binary : StreamPayloadKind::Text;
                ev.payload = payload;
                ev.offsetMs = offsetMs();
                sink.onStreamEvent(ev);
                ++seq;
                bytes += payload.size();
                frameBuf.clear();
                frameFlags = 0;
            }
        }
        if (!running) break;

        // 3) Graceful close requested (UI Disconnect / cancel) -> send CLOSE, then stop.
        bool doClose = false;
        int reqCode = 1000;
        std::string reqReason;
        {
            std::lock_guard<std::mutex> lk(session->mu);
            if (session->wantClose) { doClose = true; reqCode = session->closeCode; reqReason = session->closeReason; }
        }
        if (doClose) {
            std::vector<std::uint8_t> payload;
            payload.push_back((std::uint8_t)((reqCode >> 8) & 0xFF));
            payload.push_back((std::uint8_t)(reqCode & 0xFF));
            payload.insert(payload.end(), reqReason.begin(), reqReason.end());
            std::size_t sent = 0;
            curl_ws_send(curl, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);
            sentClose = true;
            endStatus = StreamStatus::Ok;
            endCode = reqCode;
            endMsg = reqReason;
            break;
        }

        // 4) Drain outbound queue (one frame at a time; CURLE_AGAIN -> retry next tick).
        while (running) {
            OutFrame f;
            {
                std::lock_guard<std::mutex> lk(session->mu);
                if (session->out.empty()) break;
                f = session->out.front();
            }
            std::size_t sent = 0;
            CURLcode sr = curl_ws_send(curl, f.data.data(), f.data.size(), &sent, 0, f.flags);
            if (sr == CURLE_AGAIN) break;   // socket not writable now -> keep queued, try later
            {
                std::lock_guard<std::mutex> lk(session->mu);
                if (!session->out.empty()) {
                    session->outBytes -= session->out.front().data.size();
                    session->out.pop_front();
                }
            }
            if (sr != CURLE_OK) {
                endStatus = StreamStatus::Error;
                endCode = 1006;
                endMsg = std::string("send failed: ") + curl_easy_strerror(sr);
                running = false;
                break;
            }
            lastActivity = nowMs();
            bool binary = (f.flags & CURLWS_BINARY) != 0;
            if (f.flags & (CURLWS_TEXT | CURLWS_BINARY)) {   // log only data frames (not ping/close)
                std::string payload = frameEnvelope(StreamDirection::Outbound, binary, offsetMs(),
                                                    f.data.data(), f.data.size());
                StreamEvent ev;
                ev.seq = seq;
                ev.direction = StreamDirection::Outbound;
                ev.frameType = binary ? StreamFrameType::Binary : StreamFrameType::Text;
                ev.kind = binary ? StreamPayloadKind::Binary : StreamPayloadKind::Text;
                ev.payload = payload;
                ev.offsetMs = offsetMs();
                sink.onStreamEvent(ev);
                ++seq;
                bytes += payload.size();
            }
        }
        if (!running) break;

        // 5) Keepalive PING (libcurl never pings on its own, §3.1).
        if (cfg.pingIntervalMs > 0 && nowMs() - lastPing >= cfg.pingIntervalMs) {
            std::size_t sent = 0;
            curl_ws_send(curl, "", 0, &sent, 0, CURLWS_PING);
            lastPing = nowMs();
        }

        // 6) Idle timeout -> dead connection.
        if (cfg.idleTimeoutMs > 0 && nowMs() - lastActivity >= cfg.idleTimeoutMs) {
            endStatus = StreamStatus::Timeout;
            endCode = 1006;
            endMsg = "idle timeout";
            break;
        }
    }

    (void)sentClose;
    closeWith(endStatus, endCode, endMsg);   // curl + hdrs freed by their RAII guards on return (L13)
}

} // namespace core
