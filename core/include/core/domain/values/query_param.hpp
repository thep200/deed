#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/values/key_value_entry.hpp"

namespace core::domain {

struct QueryParamTag {};

// Invariant: when enabled, key must be non-empty. Values may be empty (e.g. `?flag=`).
class QueryParam : public detail::KeyValueEntry<QueryParamTag> {
public:
  static Result<QueryParam> create(std::string key, std::string value, bool enabled = true) {
    if (enabled && key.empty())
      return Result<QueryParam>::fail(
          {ErrorCode::Validation, "query param key must not be empty", "params.key"});
    return Result<QueryParam>::ok(QueryParam(std::move(key), std::move(value), enabled));
  }

private:
  QueryParam(std::string k, std::string v, bool en) : KeyValueEntry(std::move(k), std::move(v), en) {}
};

class QueryParamList {
public:
  QueryParamList() = default;
  explicit QueryParamList(std::vector<QueryParam> items) : items_(std::move(items)) {}

  const std::vector<QueryParam> &items() const noexcept { return items_; }
  std::size_t size() const noexcept { return items_.size(); }
  bool empty() const noexcept { return items_.empty(); }

  std::vector<QueryParam> enabledOnly() const {
    std::vector<QueryParam> out;
    for (const auto &p : items_)
      if (p.enabled()) out.push_back(p);
    return out;
  }

  bool operator==(const QueryParamList &o) const { return items_ == o.items_; }
  bool operator!=(const QueryParamList &o) const { return !(*this == o); }

private:
  std::vector<QueryParam> items_;
};

} // namespace core::domain
