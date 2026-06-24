// core/streaming/sse_parser.hpp — pure text/event-stream parser (SPEC_sse §3, WHATWG EventSource).
// A buffered state machine: feed it arbitrary byte chunks, it emits completed events. It depends on
// NOTHING transport-related (no libcurl) — that keeps INV-1 and lets it be unit-tested directly (AC-7).
#pragma once

#include <functional>
#include <string>

namespace core {

// One dispatched SSE event (after joining multi-line data:, default event type "message").
struct SseEvent {
    std::string event;   // the `event:` field ("" -> caller substitutes "message")
    std::string id;      // current lastEventId at dispatch time (may be "")
    std::string data;    // joined `data:` lines, trailing '\n' removed
};

class SseParser {
public:
    using Emit = std::function<void(const SseEvent&)>;

    // Feed a chunk; calls `emit` once per completed event. A chunk may cut across lines/events — the
    // remainder is buffered for the next feed. Also processes `: comments`, id:, retry:.
    void feed(const char* data, std::size_t n, const Emit& emit);
    void feed(const std::string& s, const Emit& emit) { feed(s.data(), s.size(), emit); }

    const std::string& lastEventId() const { return lastEventId_; }   // for Last-Event-ID on reconnect
    long retryMs() const { return retryMs_; }                          // server `retry:` (-1 if unset)

    // Cap one event's data buffer (SSE_MAX_EVENT_BYTES); 0 = unlimited. Over cap -> truncated flag set.
    void setMaxEventBytes(std::size_t cap) { maxEventBytes_ = cap; }
    bool truncated() const { return truncated_; }

private:
    void onLine(const std::string& line, const Emit& emit);
    void dispatch(const Emit& emit);

    std::string buf_;          // bytes not yet forming a complete line
    std::string eventType_;    // current `event:` accumulation
    std::string dataBuf_;      // current `data:` accumulation (each line + '\n')
    std::string lastEventId_;  // persists across events (per spec)
    long retryMs_ = -1;
    std::size_t maxEventBytes_ = 0;
    bool bomChecked_ = false;
    bool truncated_ = false;
};

} // namespace core
