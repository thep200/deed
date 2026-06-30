// core/domain/common/error.hpp — neutral error type carried across layers (REFACTOR_SPEC §4.1).
// Pure C++/STL only: NO nlohmann/cpr/grpc/protobuf may ever appear under core/include/core/domain/**.
#pragma once

#include <string>

namespace core::domain {

// Stable, transport-neutral classification of a failure. Infra converts library exceptions into one of
// these at the boundary; domain/app never throw across layers (they return Result/Status, §4.1).
enum class ErrorCode {
  Ok = 0,
  Validation, // a value object invariant was violated (bad input)
  NotFound,   // entity missing (request id / environment / file)
  Conflict,   // name/path collision, optimistic-concurrency clash
  Unsupported,// operation not valid for this request type
  Parse,      // malformed serialized form (JSON, descriptor, …)
  Network,    // connection refused / DNS / reset
  Timeout,    // deadline exceeded
  Tls,        // certificate / handshake failure
  Cancelled,  // cooperatively cancelled
  Internal    // unexpected / bug
};

// A failure with a developer-facing message and an optional offending field path (e.g. "http.url").
struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message; // for dev/log, not necessarily user-facing
  std::string field;   // optional: dotted path of the field that caused it

  Error() = default;
  Error(ErrorCode c, std::string m, std::string f = {})
      : code(c), message(std::move(m)), field(std::move(f)) {}

  bool operator==(const Error &o) const {
    return code == o.code && message == o.message && field == o.field;
  }
  bool operator!=(const Error &o) const { return !(*this == o); }
};

} // namespace core::domain
