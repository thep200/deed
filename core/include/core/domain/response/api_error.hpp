#pragma once

#include <optional>
#include <string>

namespace core::domain {

enum class ErrorKind { Network, Timeout, Tls, Cancelled, Parse, Unsupported, Protocol, Internal };

inline std::string toString(ErrorKind k) {
  switch (k) {
  case ErrorKind::Network: return "network";
  case ErrorKind::Timeout: return "timeout";
  case ErrorKind::Tls: return "tls";
  case ErrorKind::Cancelled: return "cancelled";
  case ErrorKind::Parse: return "parse";
  case ErrorKind::Unsupported: return "unsupported";
  case ErrorKind::Protocol: return "protocol";
  case ErrorKind::Internal: return "internal";
  }
  return "internal";
}

struct ApiError {
  ErrorKind kind = ErrorKind::Internal;
  std::string message;
  std::optional<int> statusCode;
  bool operator==(const ApiError &o) const {
    return kind == o.kind && message == o.message && statusCode == o.statusCode;
  }
};

} // namespace core::domain
