// core/domain/values/key_value_entry.hpp — internal shared shape for the (key,value,enabled) value
// objects (REFACTOR_SPEC §5.1). Header / QueryParam / PathVariable are DISTINCT public types (they make
// implicit concepts explicit, DDD) but share storage/accessors/equality via this Tag-parameterised base
// to avoid duplication. Each public type derives and adds its OWN create() with its OWN invariants.
#pragma once

#include <string>
#include <utility>

namespace core::domain::detail {

template <class Tag> class KeyValueEntry {
public:
  const std::string &key() const noexcept { return key_; }
  const std::string &value() const noexcept { return value_; }
  bool enabled() const noexcept { return enabled_; }

  bool operator==(const KeyValueEntry &o) const {
    return key_ == o.key_ && value_ == o.value_ && enabled_ == o.enabled_;
  }
  bool operator!=(const KeyValueEntry &o) const { return !(*this == o); }

protected:
  KeyValueEntry(std::string k, std::string v, bool en)
      : key_(std::move(k)), value_(std::move(v)), enabled_(en) {}

  std::string key_;
  std::string value_;
  bool enabled_;
};

} // namespace core::domain::detail
