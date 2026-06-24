// ws_sender.hpp — WebSocket transport over libcurl (SPEC_websocket §3). INTERNAL (core/src): may leak
// libcurl; never include from core/include. One I/O thread OWNS the easy handle (curl is not thread-safe):
// it runs the recv/send pump, reassembles frames, sends keepalive PINGs, and performs graceful close.
// The UI-facing send side is a WsChannel (IStreamChannel) that only enqueues frames + wakes the I/O thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/streaming/i_stream_channel.hpp"
#include "core/streaming/i_stream_sink.hpp"
#include "core/types.hpp"

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
void wsRun(const ResolvedRequest& req, IStreamSink& sink,
           const std::shared_ptr<WsSession>& session, const std::string& sessionId);

// Request a graceful close from another thread (UI/Engine). Idempotent; wakes the I/O thread.
void wsRequestClose(const std::shared_ptr<WsSession>& session, int code, const std::string& reason);

} // namespace core
