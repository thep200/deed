// The SEND side of a duplex session; pairs with IStreamSink. Consumers depend only on this interface —
// never on a transport type.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

class IStreamChannel {
public:
    virtual ~IStreamChannel() = default;

    // Returns false when the send queue is full (backpressure) — the caller decides whether to wait or
    // drop. Thread-safe: only enqueues.
    virtual bool sendText(const std::string& utf8) = 0;
    virtual bool sendBinary(const std::vector<std::uint8_t>& bytes) = 0;

    // Graceful close (CLOSE frame, then the close handshake). Idempotent.
    virtual void close(int code = 1000, const std::string& reason = "") = 0;

    virtual bool isOpen() const = 0;
};

} // namespace core
