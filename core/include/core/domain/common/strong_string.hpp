#pragma once

#include <functional>
#include <string>
#include <utility>

namespace core::domain {

// Phantom-tagged string newtype: different Tags are distinct types that never convert to each other.
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

namespace std {
template <class Tag> struct hash<core::domain::StrongString<Tag>> {
  size_t operator()(const core::domain::StrongString<Tag> &s) const noexcept {
    return std::hash<std::string>{}(s.get());
  }
};
} // namespace std
