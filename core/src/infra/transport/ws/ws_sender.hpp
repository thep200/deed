// May leak libcurl — never include from core/include. One I/O thread OWNS the easy handle (curl is not
// thread-safe): it runs the recv/send pump, reassembles frames, pings, and closes; the UI-facing channel
// only enqueues frames for the I/O thread's next tick.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "core/domain/ws/websocket_request.hpp"
#include "infra/transport/typed_sender.hpp"
#include "infra/transport/shared/cancel_token.hpp"
#include "infra/transport/shared/i_stream_channel.hpp"
#include "infra/transport/shared/i_stream_sink.hpp"

namespace core {

// Defaults here; the composition root overrides them from .env.
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

std::shared_ptr<WsSession> wsMakeSession(const WsConfig& cfg);
std::shared_ptr<IStreamChannel> wsMakeChannel(const std::shared_ptr<WsSession>& session);

// Runs one session on the calling (I/O) thread: handshake -> pump -> close. Sink callbacks arrive on this
// background thread (the sink marshals to its UI thread); auth is applied on the handshake.
void wsRun(const core::domain::WebSocketRequest& req, IStreamSink& sink,
           const std::shared_ptr<WsSession>& session, const std::string& sessionId);

// Graceful close from another thread. Idempotent; the I/O thread honors it on its next tick.
void wsRequestClose(const std::shared_ptr<WsSession>& session, int code, const std::string& reason);

// For protocols that interpret the frames themselves (e.g. graphql-transport-ws): onOpen after the
// handshake, onText per inbound text frame, onClose exactly once (incl. handshake failure) — the protocol
// owns the open/close contract on its own sink. Outbound frames are NOT logged in this mode.
struct WsFrameHooks {
    std::function<void()> onOpen;
    std::function<void(const std::string&)> onText;
    std::function<void(StreamStatus, int, const std::string&)> onClose;
};

// Like wsRun, but drives `hooks` instead of emitting a frame-log stream. Honors `cancel` (graceful close).
void wsRunProtocol(const core::domain::WebSocketRequest& req, const WsFrameHooks& hooks,
                   const std::shared_ptr<WsSession>& session, const std::string& sessionId,
                   const std::shared_ptr<CancelToken>& cancel);

} // namespace core

namespace core::infra {

// execute() BLOCKS for the whole session (keeping the saga alive so push/close from other threads reach
// the channel). The .env tunables arrive as a pre-built WsConfig base; per-request TLS/timeout are
// layered on at execute time. One session per adapter.
class WsSenderAdapter final : public TypedSender<domain::WebSocketRequest> {
public:
  explicit WsSenderAdapter(core::WsConfig base) : base_(base) {}

  domain::Status push(domain::WsMessage) override;
  domain::Status close(int code, std::string reason) override;

protected:
  domain::Status executeTyped(const domain::RequestModel &resolved,
                              const domain::WebSocketRequest &ws, domain::IResponseSink &sink,
                              const domain::ICancellationToken &cancel) override;
  const char *mismatchMessage() const override { return "not a websocket request"; }

private:
  core::WsConfig base_;                            // .env tunables; per-request TLS/timeout layered at execute
  std::uint64_t nextId_ = 1;                       // session-id counter (one session at a time)
  std::mutex mu_;                                  // guards channel_ (set by execute, read by push/close)
  std::shared_ptr<core::IStreamChannel> channel_;  // send side of the active session
};

} // namespace core::infra
