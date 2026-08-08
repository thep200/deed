#pragma once

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/values/key_value_entry.hpp"

namespace core::domain {

struct PathVariableTag {};

// An enabled key must be a non-empty identifier (binds a `:key` URL segment); the url<->variable cross-check is a use-case-layer concern.
class PathVariable : public detail::KeyValueEntry<PathVariableTag> {
public:
  static Result<PathVariable> create(std::string key, std::string value, bool enabled = true) {
    if (enabled) {
      if (key.empty())
        return Result<PathVariable>::fail(
            {ErrorCode::Validation, "path variable key must not be empty", "pathVariables.key"});
      if (!isIdentifier(key))
        return Result<PathVariable>::fail(
            {ErrorCode::Validation, "invalid path variable key: " + key, "pathVariables.key"});
    }
    return Result<PathVariable>::ok(PathVariable(std::move(key), std::move(value), enabled));
  }

private:
  PathVariable(std::string k, std::string v, bool en) : KeyValueEntry(std::move(k), std::move(v), en) {}

  static bool isIdentifier(const std::string &s) {
    for (unsigned char c : s)
      if (!std::isalnum(c) && c != '_' && c != '-') return false;
    return true;
  }
};

class PathVariableList {
public:
  PathVariableList() = default;
  explicit PathVariableList(std::vector<PathVariable> items) : items_(std::move(items)) {}

  const std::vector<PathVariable> &items() const noexcept { return items_; }
  std::size_t size() const noexcept { return items_.size(); }
  bool empty() const noexcept { return items_.empty(); }

  bool operator==(const PathVariableList &o) const { return items_ == o.items_; }
  bool operator!=(const PathVariableList &o) const { return !(*this == o); }

private:
  std::vector<PathVariable> items_;
};

} // namespace core::domain
