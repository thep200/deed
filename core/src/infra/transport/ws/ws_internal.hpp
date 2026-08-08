#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>

#include "infra/transport/shared/socket_abort.hpp"
#include "infra/transport/ws/ws_sender.hpp"

namespace core {

struct OutFrame {
    std::vector<std::uint8_t> data;
    unsigned flags;   // CURLWS_TEXT | CURLWS_BINARY | CURLWS_CLOSE | CURLWS_PING
    int closeCode = 1000;
};

struct WsSession {
    WsConfig cfg;
    std::mutex mu;
    std::deque<OutFrame> out;
    std::uint64_t outBytes = 0;
    bool wantClose = false;
    int closeCode = 1000;
    std::string closeReason;
    std::atomic<bool> open{false};
    std::atomic<bool> done{false};
    // Handshake escape hatch: while curl_easy_perform is still connecting nothing polls wantClose, so a
    // close/cancel request shuts the socket down instead. Owned here so any thread can reach it.
    std::shared_ptr<SocketAbort> sockets = std::make_shared<SocketAbort>();
};

namespace ws_detail {

inline long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline std::string base64(const std::uint8_t* data, std::size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out.push_back(T[(v >> 18) & 0x3F]);
        out.push_back(T[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? T[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < n ? T[v & 0x3F] : '=');
    }
    return out;
}

bool enqueue(const std::shared_ptr<WsSession>& s, OutFrame f);

std::string frameEnvelope(StreamDirection dir, bool binary, long long offMs,
                          const std::uint8_t* data, std::size_t n);

struct WsResult {
    StreamStatus status = StreamStatus::Ok;
    int code = 1000;
    std::string msg;
    std::uint64_t seq = 0;
    std::uint64_t bytes = 0;
};

// Exactly one of {sink, hooks} is set: `sink` = the default frame-log stream; `hooks` = a protocol
// interprets frames itself. `cancel` (optional) stops the pump with a graceful close.
struct WsPumpIO {
    IStreamSink* sink = nullptr;
    const WsFrameHooks* hooks = nullptr;
    CancelToken* cancel = nullptr;
};

// Post-handshake pump: one tick = recv inbound, honor a close request, drain the outbound queue,
// keepalive-ping, idle-check. The CURL handle is BORROWED (freed by the caller's RAII guards).
class WsPump {
public:
    WsPump(CURL* curl, curl_socket_t sock, const std::shared_ptr<WsSession>& session,
           long long t0, const WsPumpIO& io)
        : curl_(curl), sock_(sock), session_(session), io_(io), cfg_(session->cfg), t0_(t0),
          lastPing_(nowMs()), lastActivity_(nowMs()) {}

    WsResult run();

private:
    long long offsetMs() const { return nowMs() - t0_; }

    void fail(int code, std::string msg);

    bool cancelRequested() const { return io_.cancel && io_.cancel->cancelled(); }

    void emitData(StreamDirection dir, unsigned flags, const std::uint8_t* data, std::size_t n);
    void waitReadable();
    void onInboundClose(const char* buf, std::size_t nread);
    bool accumulateFrame(const char* buf, std::size_t nread, unsigned f, const struct curl_ws_frame* m);
    void drainInbound();
    void awaitCloseHandshake();
    bool handleCloseRequest();
    void drainOutbound();
    void keepalive();
    bool idleExpired();

    CURL* curl_;
    curl_socket_t sock_;
    std::shared_ptr<WsSession> session_;
    WsPumpIO io_;
    const WsConfig& cfg_;
    long long t0_;
    long long lastPing_;
    long long lastActivity_;
    std::uint64_t seq_ = 0;
    std::uint64_t bytes_ = 0;
    std::string frameBuf_;       // inbound reassembly (per frame, until bytesleft==0)
    unsigned frameFlags_ = 0;
    bool running_ = true;
    WsResult result_;
};

} // namespace ws_detail
} // namespace core
