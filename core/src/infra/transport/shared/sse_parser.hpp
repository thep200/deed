// Pure text/event-stream parser (WHATWG EventSource): feed arbitrary byte chunks, it emits completed
// events. No transport deps (no libcurl), so it unit-tests directly.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

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

    // A chunk may cut across lines/events — the remainder is buffered. Also processes `: comments`, id:, retry:.
    void feed(const char* data, std::size_t n, const Emit& emit);
    void feed(const std::string& s, const Emit& emit) { feed(s.data(), s.size(), emit); }

    // Flush on clean EOF: a final line/event left in the buffer (no trailing newline / lone '\r') still dispatches.
    void finish(const Emit& emit);

    const std::string& lastEventId() const { return lastEventId_; }   // for Last-Event-ID on reconnect
    void setLastEventId(std::string id) { lastEventId_ = std::move(id); }  // seed on reconnect (persists per spec)
    long retryMs() const { return retryMs_; }                          // server `retry:` (-1 if unset)

    // Cap one event's data buffer (SSE_MAX_EVENT_BYTES); 0 = unlimited. Over cap -> truncated flag set.
    void setMaxEventBytes(std::size_t cap) { maxEventBytes_ = cap; }
    bool truncated() const { return truncated_; }

private:
    void onLine(std::string_view line, const Emit& emit);
    void handleDataField(std::string_view value); // accumulate `data:` honoring the byte cap
    void handleRetryField(std::string_view value); // parse + clamp `retry:`
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
