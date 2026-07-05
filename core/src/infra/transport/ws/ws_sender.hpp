// ws_sender.hpp — WebSocket transport over libcurl (SPEC_websocket §3). INTERNAL (core/src): may leak
// libcurl; never include from core/include. One I/O thread OWNS the easy handle (curl is not thread-safe):
// it runs the recv/send pump, reassembles frames, sends keepalive PINGs, and performs graceful close.
// The UI-facing send side is a WsChannel (IStreamChannel) that only enqueues frames; the I/O thread
// picks them up on its next tick.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "core/domain/ports/driven/i_request_sender.hpp"
#include "core/domain/ws/websocket_request.hpp"
#include "infra/transport/shared/cancel_token.hpp"
#include "infra/transport/shared/i_stream_channel.hpp"
#include "infra/transport/shared/i_stream_sink.hpp" // StreamStatus / StreamEvent / IStreamSink (infra-internal)

namespace core {

// WS tunables (SPEC_websocket §9). Defaults here; Engine overrides from .env via EngineConfig.
struct WsConfig {
    int pingIntervalMs = 20000;     // active PING cadence (libcurl never pings on its own)
    int idleTimeoutMs = 60000;      // no traffic for this long -> treat as dead, close
    int closeTimeoutMs = 3000;      // wait this long for the close handshake
    int connectTimeoutMs = 30000;   // handshake timeout
    std::uint64_t maxFrameBytes = 16ull * 1024 * 1024;     // cap one reassembled inbound frame
    std::size_t sendQueueMaxFrames = 1024;                 // backpressure: max queued outbound frames
    std::uint64_t sendQueueMaxBytes = 32ull * 1024 * 1024; // backpressure: max queued outbound bytes
    bool verifyTls = true;          // wss:// cert verification (always on; no app-global toggle)
};

// Shared duplex state between the UI-facing channel and the I/O thread. Opaque (defined in the .cpp).
struct WsSession;

// Create the shared session state + the send-side channel the UI holds.
std::shared_ptr<WsSession> wsMakeSession(const WsConfig& cfg);
std::shared_ptr<IStreamChannel> wsMakeChannel(const std::shared_ptr<WsSession>& session);

// Run one session on the calling (I/O) thread: handshake -> recv/send pump -> close. Emits
// onStreamOpen/Event/Close on `sink` (background thread; the sink marshals to its UI thread).
// Takes the resolved domain WebSocketRequest directly (no legacy struct); auth is applied on the handshake.
void wsRun(const core::domain::WebSocketRequest& req, IStreamSink& sink,
           const std::shared_ptr<WsSession>& session, const std::string& sessionId);

// Request a graceful close from another thread (UI/Engine). Idempotent; the I/O thread honors it on its
// next tick.
void wsRequestClose(const std::shared_ptr<WsSession>& session, int code, const std::string& reason);

// Frame-level hooks for a protocol that interprets the WS frames itself (e.g. graphql-transport-ws)
// instead of the default frame-log stream. The pump calls onOpen after a successful handshake, onText for
// each inbound text frame, and onClose exactly once at the end (incl. handshake failure) — so the protocol
// owns the §3 open/close contract on its own sink. Outbound frames are NOT logged in this mode.
struct WsFrameHooks {
    std::function<void()> onOpen;
    std::function<void(const std::string&)> onText;
    std::function<void(StreamStatus, int, const std::string&)> onClose;
};

// Like wsRun, but drives `hooks` instead of emitting a frame-log stream. Honors `cancel` (graceful close).
// Used by the GraphQL-over-WebSocket sender.
void wsRunProtocol(const core::domain::WebSocketRequest& req, const WsFrameHooks& hooks,
                   const std::shared_ptr<WsSession>& session, const std::string& sessionId,
                   const std::shared_ptr<CancelToken>& cancel);

} // namespace core

namespace core::infra {

// Native WebSocket sender (domain IRequestSender). Runs the libcurl pump above DIRECTLY on the domain
// model — execute() runs wsRun on the calling (saga pool) thread, which BLOCKS for the whole session
// (keeping the saga alive so push/close from other threads reach the channel); inbound frames become
// EvMessage, the close becomes EvClosed/EvFailed. Auth is applied by the pump (applyAuthHeaders on the
// handshake), so the already-resolved domain model is enough. The .env WS tunables arrive as a pre-built
// WsConfig base; per-request TLS/timeout from RequestConfig are layered on at execute time. One session
// per adapter. (Merged here from the former ws_sender_adapter.* — RESTRUCTURE_PLAN S3.)
class WsSenderAdapter final : public domain::IRequestSender {
public:
  explicit WsSenderAdapter(core::WsConfig base) : base_(base) {}

  bool supports(domain::RequestType t) const override { return t == domain::RequestType::WebSocket; }
  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;
  domain::Status push(domain::WsMessage) override;
  domain::Status close(int code, std::string reason) override;

private:
  core::WsConfig base_;                            // .env tunables; per-request TLS/timeout layered at execute
  std::uint64_t nextId_ = 1;                       // session-id counter (one session at a time)
  std::mutex mu_;                                  // guards channel_ (set by execute, read by push/close)
  std::shared_ptr<core::IStreamChannel> channel_;  // send side of the active session
};

} // namespace core::infra
