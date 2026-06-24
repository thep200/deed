// core/response.hpp — neutral result/handle DTOs returned to the UI (README §7).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/dto_common.hpp"

namespace core {

struct Cookie {
  std::string name;
  std::string value;
  std::string domain;
  std::string path;
  std::string expires;
};

struct ApiResponse {
  // HTTP
  int statusCode = 0;
  std::string statusText;
  std::vector<KeyValue> headers;
  std::vector<Cookie> cookies; // Set-Cookie of the current response (POC, no jar)
  // common
  std::string body; // HTTP body or gRPC message JSON
  long elapsedMs = 0;
  std::int64_t sizeBytes = 0;
  std::string resolvedRequestDump; // resolved request (Request tab for debugging)
  // --- Streaming (SPEC_grpc_streaming §8): set when body is an assembled stream array ---
  bool wasStreamed = false; // true -> body is the captured [ … ] array
  bool partial = false;     // true -> stream ended early (cancel/error) -> array incomplete
  std::uint64_t eventCount = 0; // number of events captured into the array
};

enum class ErrorKind { Network, Timeout, Tls, Cancelled, Parse, Unsupported, Unknown };
std::string toString(ErrorKind);

struct ApiError {
  ErrorKind kind = ErrorKind::Unknown;
  std::string message;
};

// ---- Lightweight JSON validate (UI spec §7) ----
struct ValidationResult {
  bool ok = true;
  int line = 0;
  int col = 0;
  std::string msg;
};

// Handle to track/cancel an in-flight request (README §3 threading).
using RequestHandle = std::uint64_t;

struct Progress {
  std::int64_t downloadTotal = 0;
  std::int64_t downloadNow = 0;
  std::int64_t uploadTotal = 0;
  std::int64_t uploadNow = 0;
};

} // namespace core
