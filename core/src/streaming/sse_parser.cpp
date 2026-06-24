// sse_parser.cpp — pure text/event-stream parser (SPEC_sse §3). No transport deps.
#include "core/streaming/sse_parser.hpp"

#include <algorithm>

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

    // Split complete lines. Terminators: "\n", "\r\n", or a lone "\r". A trailing '\r' at the end of the
    // buffer is ambiguous (could be the CR of a CRLF split across chunks) -> keep it for the next feed.
    std::size_t i = 0, lineStart = 0;
    while (i < buf_.size()) {
        char c = buf_[i];
        if (c == '\n') {
            onLine(buf_.substr(lineStart, i - lineStart), emit);
            lineStart = ++i;
        } else if (c == '\r') {
            if (i + 1 >= buf_.size()) break;     // trailing CR -> defer (maybe CRLF next chunk)
            onLine(buf_.substr(lineStart, i - lineStart), emit);
            i += (buf_[i + 1] == '\n') ? 2 : 1;  // CRLF or lone CR
            lineStart = i;
        } else {
            ++i;
        }
    }
    buf_.erase(0, lineStart);
}

void SseParser::finish(const Emit& emit) {
    // EOF: process whatever is left as the final line (M12). feed() defers a trailing '\r' (it could be the
    // CR of a CRLF split across chunks) — at a clean EOF that ambiguity is resolved, so strip it and emit.
    if (!buf_.empty()) {
        std::string line = buf_;
        if (line.back() == '\r') line.pop_back();
        if (!line.empty()) onLine(line, emit);
        buf_.clear();
    }
    dispatch(emit);   // flush a final event that had no terminating blank line
}

void SseParser::onLine(const std::string& line, const Emit& emit) {
    if (line.empty()) { dispatch(emit); return; }   // blank line -> dispatch
    if (line[0] == ':') return;                      // comment / heartbeat -> ignore (idle reset is upstream)

    std::size_t colon = line.find(':');
    std::string field, value;
    if (colon == std::string::npos) {
        field = line;                                // field with no value
    } else {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);   // strip ONE leading space
    }

    if (field == "event") {
        eventType_ = value;
    } else if (field == "data") {
        if (maxEventBytes_ != 0 && dataBuf_.size() >= maxEventBytes_) {
            truncated_ = true;                       // cap hit -> stop accumulating (no OOM)
        } else {
            dataBuf_ += value;
            dataBuf_ += '\n';
            if (maxEventBytes_ != 0 && dataBuf_.size() > maxEventBytes_) {
                dataBuf_.resize(maxEventBytes_);
                truncated_ = true;
            }
        }
    } else if (field == "id") {
        if (value.find('\0') == std::string::npos) lastEventId_ = value;  // empty id is valid
    } else if (field == "retry") {
        bool allDigit = !value.empty();
        for (char c : value) if (c < '0' || c > '9') { allDigit = false; break; }
        // Clamp to a sane ceiling (M11): a hostile/huge `retry:` must not make the I/O thread sleep for
        // days (cancel latency) — overflow from stol also lands on the max.
        if (allDigit) {
            try { long v = std::stol(value); retryMs_ = v < 0 ? 0 : (v > 60000 ? 60000 : v); }
            catch (...) { retryMs_ = 60000; }
        }
    }
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
