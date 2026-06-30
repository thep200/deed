// core/domain/common/enabled_flag.hpp — EnabledFlag (REFACTOR_SPEC §5.1): a tiny VO over a bool that
// also knows the JSON 0/1 convention used by collection files (mapping itself lives in infra, but the
// 0/1 <-> bool helpers are pure and handy to share).
#pragma once

namespace core::domain {

class EnabledFlag {
public:
  constexpr EnabledFlag() = default;
  constexpr explicit EnabledFlag(bool on) : on_(on) {}

  static constexpr EnabledFlag enabled() { return EnabledFlag(true); }
  static constexpr EnabledFlag disabled() { return EnabledFlag(false); }
  // JSON stores enabled as 0/1; treat any non-zero as enabled.
  static constexpr EnabledFlag fromInt(int v) { return EnabledFlag(v != 0); }

  constexpr bool value() const { return on_; }
  constexpr int toInt() const { return on_ ? 1 : 0; }
  constexpr explicit operator bool() const { return on_; }

  constexpr bool operator==(const EnabledFlag &o) const { return on_ == o.on_; }
  constexpr bool operator!=(const EnabledFlag &o) const { return on_ != o.on_; }

private:
  bool on_ = true;
};

} // namespace core::domain
