// core/streaming/i_stream_channel.hpp — the SEND side of a duplex session (SPEC_websocket §2.2).
// Pairs with IStreamSink (receive): IStreamChannel + IStreamSink = a full-duplex session. The UI holds
// a channel to push frames into an open session; it depends only on this interface + StreamEvent — never
// on a transport type (libcurl/curl_ws) — keeping INV-1. Reused as-is for gRPC bidi v2.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

class IStreamChannel {
public:
    virtual ~IStreamChannel() = default;

    // Queue a frame to send. Returns false if the send queue is full (backpressure §11) — the caller
    // decides whether to wait or drop. Thread-safe: callable from the UI thread (only enqueues).
    virtual bool sendText(const std::string& utf8) = 0;
    virtual bool sendBinary(const std::vector<std::uint8_t>& bytes) = 0;

    // Request a graceful close (sends a CLOSE frame, then waits for the close handshake). Idempotent.
    virtual void close(int code = 1000, const std::string& reason = "") = 0;

    // Is the session currently open (handshake done, not yet closed)?
    virtual bool isOpen() const = 0;
};

} // namespace core
