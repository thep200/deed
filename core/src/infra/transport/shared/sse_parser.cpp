#include "infra/transport/shared/sse_parser.hpp"

#include <algorithm>
#include <string_view>

namespace core {

void SseParser::feed(const char* data, std::size_t n, const Emit& emit) {
    buf_.append(data, n);

    // Strip a leading UTF-8 BOM once (may be split across the first chunks).
    if (!bomChecked_) {
        static const unsigned char kBom[3] = {0xEF, 0xBB, 0xBF};
        std::size_t k = std::min<std::size_t>(buf_.size(), 3);
        bool prefix = true;
        for (std::size_t j = 0; j < k; ++j)
            if (static_cast<unsigned char>(buf_[j]) != kBom[j]) { prefix = false; break; }
        if (prefix && k < 3) return;             // could still be a BOM -> wait for more bytes
        if (prefix && k == 3) buf_.erase(0, 3);  // BOM present -> drop it
        bomChecked_ = true;
    }

    // Terminators: "\n", "\r\n", lone "\r"; a trailing '\r' is ambiguous (CRLF split across chunks) -> defer.
    // Lines are views into buf_: buf_ is only mutated after the loop and emit never touches this parser.
    std::size_t i = 0, lineStart = 0;
    while (i < buf_.size()) {
        char c = buf_[i];
        if (c == '\n') {
            onLine(std::string_view(buf_.data() + lineStart, i - lineStart), emit);
            lineStart = ++i;
        } else if (c == '\r') {
            if (i + 1 >= buf_.size()) break;     // trailing CR -> defer (maybe CRLF next chunk)
            onLine(std::string_view(buf_.data() + lineStart, i - lineStart), emit);
            i += (buf_[i + 1] == '\n') ? 2 : 1;  // CRLF or lone CR
            lineStart = i;
        } else {
            ++i;
        }
    }
    buf_.erase(0, lineStart);
}

void SseParser::finish(const Emit& emit) {
    // A trailing '\r' deferred by feed() is resolved at clean EOF: strip it and emit the final line.
    if (!buf_.empty()) {
        std::string_view line = buf_;
        if (line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) onLine(line, emit);
        buf_.clear();
    }
    dispatch(emit);   // flush a final event that had no terminating blank line
}

void SseParser::handleDataField(std::string_view value) {
    if (maxEventBytes_ != 0 && dataBuf_.size() >= maxEventBytes_) {
        truncated_ = true; // cap hit -> stop accumulating (no OOM)
        return;
    }
    dataBuf_ += value;
    dataBuf_ += '\n';
    if (maxEventBytes_ != 0 && dataBuf_.size() > maxEventBytes_) {
        dataBuf_.resize(maxEventBytes_);
        truncated_ = true;
    }
}

void SseParser::handleRetryField(std::string_view value) {
    bool allDigit = !value.empty();
    for (char c : value) if (c < '0' || c > '9') { allDigit = false; break; }
    if (!allDigit) return;
    // Clamp: a hostile/huge `retry:` must not park the I/O thread (cancel latency); stol overflow also lands on max.
    try { long v = std::stol(std::string(value)); retryMs_ = v < 0 ? 0 : (v > 60000 ? 60000 : v); }
    catch (...) { retryMs_ = 60000; }
}

void SseParser::onLine(std::string_view line, const Emit& emit) {
    if (line.empty()) { dispatch(emit); return; }   // blank line -> dispatch
    if (line[0] == ':') return;                      // comment / heartbeat -> ignore (idle reset is upstream)

    std::size_t colon = line.find(':');
    std::string_view field = (colon == std::string_view::npos) ? line : line.substr(0, colon);
    std::string_view value;
    if (colon != std::string_view::npos) {
        value = line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value.remove_prefix(1);   // strip ONE leading space
    }

    if (field == "event") eventType_.assign(value);
    else if (field == "data") handleDataField(value);
    else if (field == "id") { if (value.find('\0') == std::string_view::npos) lastEventId_.assign(value); } // empty id valid
    else if (field == "retry") handleRetryField(value);
    // unknown field -> ignore (spec)
}

void SseParser::dispatch(const Emit& emit) {
    if (dataBuf_.empty()) { eventType_.clear(); return; }   // only comment/id -> no event (lastEventId kept)
    if (dataBuf_.back() == '\n') dataBuf_.pop_back();        // drop the final '\n'
    SseEvent ev;
    ev.event = eventType_;
    ev.id = lastEventId_;
    ev.data = dataBuf_;
    emit(ev);
    eventType_.clear();
    dataBuf_.clear();
    // lastEventId_ persists across events (per spec)
}

} // namespace core
