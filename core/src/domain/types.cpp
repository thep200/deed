#include "core/types.hpp"

namespace core {

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
