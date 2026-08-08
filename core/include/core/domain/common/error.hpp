#pragma once

#include <string>

namespace core::domain {

// Infra converts library exceptions into one of these at the boundary; domain/app never throw across layers.
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
