// core/domain/common/result.hpp — Result<T> (REFACTOR_SPEC §4.1): a value-or-error carrier replacing
// exceptions across layer boundaries. C++17 has no std::expected, so this is the minimal stand-in.
// Pure STL only.
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

  // Precondition: isOk(). Reading the value of a failed Result is a programming error.
  const T &value() const & { return *value_; }
  T &&take() { return std::move(*value_); } // move the value out (single use)

  const Error &error() const { return error_; }

  // Convenience: the value if ok, otherwise the supplied fallback (no precondition).
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

// Status = a fallible "void". ok() / fail(Error).
using Status = Result<std::monostate>;

inline Status ok() { return Status::ok(std::monostate{}); }
inline Status fail(Error e) { return Status::fail(std::move(e)); }

} // namespace core::domain
