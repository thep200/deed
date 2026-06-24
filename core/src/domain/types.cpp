#include "core/types.hpp"

#include <algorithm>
#include <cctype>

namespace core {

namespace {
std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
// True if an enabled `Accept` header advertises text/event-stream (the standard SSE handshake).
bool hasAcceptEventStream(const std::vector<KeyValue>& headers) {
    for (const auto& h : headers) {
        if (!h.enabled) continue;
        if (toLower(h.key) == "accept" && toLower(h.value).find("text/event-stream") != std::string::npos)
            return true;
    }
    return false;
}
} // namespace

bool httpRequestsSse(const HttpRequest& h) {
    return h.streamMode != HttpStreamMode::None || hasAcceptEventStream(h.headers);
}
bool httpForcesSse(const HttpRequest& h) {
    return h.streamMode == HttpStreamMode::Sse || hasAcceptEventStream(h.headers);
}

std::string toString(RequestType t) {
    switch (t) {
        case RequestType::Grpc: return "grpc";
        case RequestType::WebSocket: return "ws";
        default: return "http";
    }
}

bool parseRequestType(const std::string& s, RequestType& out) {
    if (s == "http") { out = RequestType::Http; return true; }
    if (s == "grpc") { out = RequestType::Grpc; return true; }
    if (s == "ws") { out = RequestType::WebSocket; return true; }
    return false;
}

std::string toString(ErrorKind k) {
    switch (k) {
        case ErrorKind::Network: return "NETWORK";
        case ErrorKind::Timeout: return "TIMEOUT";
        case ErrorKind::Tls: return "TLS";
        case ErrorKind::Cancelled: return "CANCELLED";
        case ErrorKind::Parse: return "PARSE";
        case ErrorKind::Unsupported: return "UNSUPPORTED";
        default: return "UNKNOWN";
    }
}

} // namespace core
