#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "core/domain/common/error.hpp"

namespace core::domain {

// Immutable outcome: holds EITHER a value (isOk) OR an Error. Construct only via ok()/fail().
template <class T> class Result {
public:
  static Result ok(T v) { return Result(std::move(v)); }
  static Result fail(Error e) { return Result(std::move(e)); }

  bool isOk() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return isOk(); }

  // Precondition: isOk() — the unchecked optional deref is by-contract, so the clang-tidy check is muted.
  const T &value() const & { return *value_; } // NOLINT(bugprone-unchecked-optional-access)
  T &&take() { return std::move(*value_); }    // NOLINT(bugprone-unchecked-optional-access) — move out (single use)

  const Error &error() const { return error_; }

  T valueOr(T fallback) const { return value_.has_value() ? *value_ : std::move(fallback); }

  bool operator==(const Result &o) const {
    if (isOk() != o.isOk()) return false;
    return isOk() ? (*value_ == *o.value_) : (error_ == o.error_);
  }
  bool operator!=(const Result &o) const { return !(*this == o); }

private:
  explicit Result(T v) : value_(std::move(v)) {}
  explicit Result(Error e) : error_(std::move(e)) {}
  std::optional<T> value_;
  Error error_;
};

// Status = a fallible "void".
using Status = Result<std::monostate>;

inline Status ok() { return Status::ok(std::monostate{}); }
inline Status fail(Error e) { return Status::fail(std::move(e)); }

} // namespace core::domain
