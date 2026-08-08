#include "infra/transport/ws/ws_sender.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>

#include "infra/transport/shared/socket_abort.hpp" // connect phase: no callback runs, shutdown() the fd
#include "infra/transport/ws/ws_internal.hpp"

namespace core {
namespace d = core::domain;
using namespace ws_detail;

namespace {

long long nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Runtime check: was this libcurl built with the ws/wss protocol handlers? Symbols can exist while the
// scheme is disabled. Cached after the first query.
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

// Caller owns the returned list.
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

// The pump's cancel checks only run AFTER the handshake returns — without this, a peer that accepts TCP
// then never answers the upgrade parks the thread and Cancel does nothing.
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

// CONNECT_ONLY=2 -> perform the handshake, then hand the socket back.
void configureWsHandshake(CURL* curl, const d::WebSocketRequest& w, const WsConfig& cfg,
                          struct curl_slist* hdrs, HandshakeAbort* abort, SocketAbort* sockets) {
    curl_easy_setopt(curl, CURLOPT_URL, w.url().raw().c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)cfg.connectTimeoutMs);
    // Bound the WHOLE handshake, not just the TCP/TLS connect — a peer that stalls after the connect
    // would hold the thread (CONNECT_ONLY=2 returns as soon as the upgrade completes anyway).
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)cfg.connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, cfg.verifyTls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, cfg.verifyTls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);   // small frames not held by Nagle
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, handshakeAbortCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, abort);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    SocketAbort::install(curl, sockets);
}

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

// One handshake -> pump run, shared by wsRun and wsRunProtocol. Pre-pump failure -> `onFail` + nullopt;
// otherwise `onOpen` fires once right after the handshake succeeds and the pump result is returned.
std::optional<WsResult> wsHandshakeAndPump(
        const d::WebSocketRequest& w, const std::shared_ptr<WsSession>& session, long long t0,
        const WsPumpIO& io, const std::function<void(StreamStatus, int, const std::string&)>& onFail,
        const std::function<void()>& onOpen) {
    if (!curlHasWebSocket()) {
        onFail(StreamStatus::Error, 0,
               "libcurl was built without WebSocket support (need curl[websockets], libcurl >= 8.11)");
        return std::nullopt;
    }

    // RAII: the easy handle + header slist free themselves on EVERY return path.
    auto curlDel = [](CURL* c) { if (c) curl_easy_cleanup(c); };
    std::unique_ptr<CURL, decltype(curlDel)> curlGuard(curl_easy_init(), curlDel);
    CURL* curl = curlGuard.get();
    if (!curl) {
        onFail(StreamStatus::Error, 0, "curl init failed");
        return std::nullopt;
    }

    auto slistDel = [](struct curl_slist* s) { if (s) curl_slist_free_all(s); };
    struct curl_slist* hdrs = buildWsHandshakeHeaders(w);   // auth + custom + Sec-WebSocket-Protocol
    // guard owns the list (curl_easy_setopt only borrows the pointer)
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
            sink.onStreamOpen(meta);   // contract: always one open before any close
            wsEmitClose(sink, session, WsResult{st, code, msg, 0, 0}, offsetMs());
        },
        [&] {
            sink.onStreamOpen(meta);   // contract: always one open before any close
            // onOpenSend: queue subscribe-style messages to fire right after open.
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
    // On any terminal outcome, the protocol owns the open/close contract via hooks.onClose.
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

#include <exception>
#include <variant>

namespace core::infra {
namespace d = core::domain;

namespace {
// IStreamSink -> domain ResponseEvents; lives on the stack for one wsRun() call.
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

d::Status WsSenderAdapter::executeTyped(const d::RequestModel &resolved,
                                        const d::WebSocketRequest &w, d::IResponseSink &sink,
                                        const d::ICancellationToken &cancel) {
  // Model is already {{var}}-resolved; .env base + per-request RequestConfig (TLS verify + idle timeout).
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
  // Cancel from any thread -> a close request both the pump AND a stuck handshake honor; fires
  // immediately when the caller is already cancelled.
  cancel.onCancel([session] { wsRequestClose(session, 1000, "cancelled"); });

  WsInboundTranslator translator;
  translator.sink = &sink;
  try {
    // The pump runs on THIS thread and returns after the close handshake (parks the saga's worker for the session).
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
