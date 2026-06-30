// core/domain/common/strong_string.hpp — phantom-tagged string newtype (REFACTOR_SPEC §4.4) so that
// RequestId, Token, HeaderName, … are distinct types and cannot be swapped at call sites by mistake.
#pragma once

#include <functional>
#include <string>
#include <utility>

namespace core::domain {

// StrongString<Tag>: a value-semantic wrapper around std::string distinguished only by its Tag.
// Two instances with different Tags do NOT implicitly convert to each other.
template <class Tag> class StrongString {
public:
  StrongString() = default;
  explicit StrongString(std::string v) : value_(std::move(v)) {}

  const std::string &get() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }

  bool operator==(const StrongString &o) const { return value_ == o.value_; }
  bool operator!=(const StrongString &o) const { return value_ != o.value_; }
  bool operator<(const StrongString &o) const { return value_ < o.value_; } // for use as map key

private:
  std::string value_;
};

} // namespace core::domain

// Hashable -> usable directly as an unordered_map key (orchestrator keys sagas by RequestExecutionId).
namespace std {
template <class Tag> struct hash<core::domain::StrongString<Tag>> {
  size_t operator()(const core::domain::StrongString<Tag> &s) const noexcept {
    return std::hash<std::string>{}(s.get());
  }
};
} // namespace std
