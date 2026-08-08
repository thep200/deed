#include "infra/transport/ws/ws_internal.hpp"

#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace core::ws_detail {

WsResult WsPump::run() {
    while (running_) {
        waitReadable();         // block up to one tick on readability (also paces send/ping)
        drainInbound();         // sets the result on close/error
        if (!running_) break;
        if (handleCloseRequest()) break;   // UI Disconnect/cancel -> send CLOSE, stop
        drainOutbound();
        if (!running_) break;
        keepalive();            // libcurl never pings on its own
        if (idleExpired()) break;          // no activity for too long -> dead connection
    }
    result_.seq = seq_;
    result_.bytes = bytes_;
    return result_;
}

void WsPump::fail(int code, std::string msg) {
    result_.status = StreamStatus::Error;
    result_.code = code;
    result_.msg = std::move(msg);
    running_ = false;
}

// Deliver one data frame. Protocol mode: hand raw INBOUND text to the protocol (outbound not logged).
// Default mode: build a neutral frame-log element and emit it on the sink (in + out).
void WsPump::emitData(StreamDirection dir, unsigned flags, const std::uint8_t* data, std::size_t n) {
    if (io_.hooks) {
        if (dir == StreamDirection::Inbound && io_.hooks->onText)
            io_.hooks->onText(std::string(reinterpret_cast<const char*>(data), n));
        return;
    }
    if (!io_.sink) return;
    bool binary = (flags & CURLWS_BINARY) != 0;
    std::string payload = frameEnvelope(dir, binary, offsetMs(), data, n);
    StreamEvent ev;
    ev.seq = seq_;
    ev.direction = dir;
    ev.frameType = binary ? StreamFrameType::Binary : StreamFrameType::Text;
    ev.kind = binary ? StreamPayloadKind::Binary : StreamPayloadKind::Text;
    ev.payload = std::move(payload);
    ev.offsetMs = offsetMs();
    io_.sink->onStreamEvent(ev);
    ++seq_;
    bytes_ += ev.payload.size();
}

void WsPump::waitReadable() {
    if (sock_ == CURL_SOCKET_BAD) return;
    fd_set rfd;
    FD_ZERO(&rfd);
    FD_SET(sock_, &rfd);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 50 * 1000;   // 50ms tick
    select((int)sock_ + 1, &rfd, nullptr, nullptr, &tv);
}

// A received CLOSE frame -> record the peer's code/reason and stop.
void WsPump::onInboundClose(const char* buf, std::size_t nread) {
    int code = 1005;
    std::string reason;
    if (nread >= 2) {
        code = ((unsigned char)buf[0] << 8) | (unsigned char)buf[1];
        reason.assign(buf + 2, nread - 2);
    }
    result_.status = (code == 1000) ? StreamStatus::Ok : StreamStatus::Error;
    result_.code = code;
    result_.msg = reason;
    running_ = false;
}

// Accumulate a data frame (TEXT/BINARY/CONT) until complete, then emit it. Returns false on overflow.
bool WsPump::accumulateFrame(const char* buf, std::size_t nread, unsigned f, const struct curl_ws_frame* m) {
    if (frameBuf_.empty()) frameFlags_ = f;
    frameBuf_.append(buf, nread);
    if (frameBuf_.size() > cfg_.maxFrameBytes) {
        fail(1009, "inbound frame exceeded WS_MAX_FRAME_BYTES");   // 1009 = message too big
        return false;
    }
    if (m && m->bytesleft == 0) {   // frame fully received -> emit one log element
        emitData(StreamDirection::Inbound, frameFlags_,
                 reinterpret_cast<const std::uint8_t*>(frameBuf_.data()), frameBuf_.size());
        frameBuf_.clear();
        frameFlags_ = 0;
    }
    return true;
}

// Drain all available inbound data (non-blocking; CONNECT_ONLY sockets return CURLE_AGAIN).
void WsPump::drainInbound() {
    while (running_) {
        char buf[16384];
        std::size_t nread = 0;
        const struct curl_ws_frame* m = nullptr;
        CURLcode r = curl_ws_recv(curl_, buf, sizeof(buf), &nread, &m);
        if (r == CURLE_AGAIN) break;
        if (r != CURLE_OK) {   // closed/broken without a CLOSE frame -> abnormal (1006)
            fail(1006, std::string("connection lost: ") + curl_easy_strerror(r));
            break;
        }
        lastActivity_ = nowMs();
        unsigned f = m ? (unsigned)m->flags : 0;
        if (f & CURLWS_CLOSE) { onInboundClose(buf, nread); break; }
        if (f & (CURLWS_PING | CURLWS_PONG)) continue;   // libcurl auto-PONGs; count as activity only
        if (!accumulateFrame(buf, nread, f, m)) break;
    }
}

// After our CLOSE went out, wait (bounded by closeTimeoutMs) for the peer's CLOSE echo; non-CLOSE
// frames received meanwhile are discarded.
void WsPump::awaitCloseHandshake() {
    const long long deadline = nowMs() + cfg_.closeTimeoutMs;
    while (cfg_.closeTimeoutMs > 0 && nowMs() < deadline) {
        waitReadable();
        char buf[16384];
        std::size_t nread = 0;
        const struct curl_ws_frame* m = nullptr;
        CURLcode r = curl_ws_recv(curl_, buf, sizeof(buf), &nread, &m);
        if (r == CURLE_AGAIN) continue;
        if (r != CURLE_OK) return;                       // connection gone -> nothing to wait for
        if (m && (m->flags & CURLWS_CLOSE)) return;      // peer echoed CLOSE -> handshake complete
    }
}

// Graceful close requested (UI Disconnect / wantClose / cancel) -> send CLOSE, then wait for the
// peer's echo. Returns true if we stop (false while the socket is not writable yet).
bool WsPump::handleCloseRequest() {
    bool doClose = false;
    int reqCode = 1000;
    std::string reqReason;
    {
        std::lock_guard<std::mutex> lk(session_->mu);
        if (session_->wantClose) { doClose = true; reqCode = session_->closeCode; reqReason = session_->closeReason; }
    }
    bool cancelled = cancelRequested();
    if (!doClose && cancelled) doClose = true;   // openStream cancel -> graceful close
    if (!doClose) return false;
    std::vector<std::uint8_t> payload;
    payload.push_back((std::uint8_t)((reqCode >> 8) & 0xFF));
    payload.push_back((std::uint8_t)(reqCode & 0xFF));
    payload.insert(payload.end(), reqReason.begin(), reqReason.end());
    std::size_t sent = 0;
    CURLcode sr = curl_ws_send(curl_, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);
    if (sr == CURLE_AGAIN) return false;   // socket not writable now -> retry next tick
    if (sr == CURLE_OK) awaitCloseHandshake();
    result_.status = cancelled ? StreamStatus::Cancelled : StreamStatus::Ok;
    result_.code = reqCode;
    result_.msg = reqReason;
    return true;
}

// Drain the outbound queue (one frame at a time; CURLE_AGAIN -> retry next tick). The front element
// is used through a stable pointer instead of a copy: only this thread pops, and deque::push_back
// never invalidates references to existing elements.
void WsPump::drainOutbound() {
    while (running_) {
        const OutFrame* f = nullptr;
        {
            std::lock_guard<std::mutex> lk(session_->mu);
            if (session_->out.empty()) break;
            f = &session_->out.front();
        }
        std::size_t sent = 0;
        CURLcode sr = curl_ws_send(curl_, f->data.data(), f->data.size(), &sent, 0, f->flags);
        if (sr == CURLE_AGAIN) break;   // socket not writable now -> keep queued, try later
        if (sr != CURLE_OK) { fail(1006, std::string("send failed: ") + curl_easy_strerror(sr)); break; }
        lastActivity_ = nowMs();
        if (f->flags & (CURLWS_TEXT | CURLWS_BINARY))   // log only data frames (not ping/close)
            emitData(StreamDirection::Outbound, f->flags, f->data.data(), f->data.size());
        {
            std::lock_guard<std::mutex> lk(session_->mu);
            session_->outBytes -= f->data.size();
            session_->out.pop_front();
        }
    }
}

void WsPump::keepalive() {
    if (cfg_.pingIntervalMs > 0 && nowMs() - lastPing_ >= cfg_.pingIntervalMs) {
        std::size_t sent = 0;
        curl_ws_send(curl_, "", 0, &sent, 0, CURLWS_PING);
        lastPing_ = nowMs();
    }
}

bool WsPump::idleExpired() {
    if (cfg_.idleTimeoutMs > 0 && nowMs() - lastActivity_ >= cfg_.idleTimeoutMs) {
        result_.status = StreamStatus::Timeout;
        result_.code = 1006;
        result_.msg = "idle timeout";
        return true;
    }
    return false;
}

} // namespace core::ws_detail
