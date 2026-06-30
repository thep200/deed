// core/domain/values/header.hpp — Header value object + HeaderList (REFACTOR_SPEC §5.1).
#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/values/key_value_entry.hpp"

namespace core::domain {

struct HeaderTag {};

// Invariant: when enabled, name must be a non-empty, valid HTTP field-name token (RFC 7230 tchar).
// A disabled header may have an empty name (it is ignored on send).
class Header : public detail::KeyValueEntry<HeaderTag> {
public:
  static Result<Header> create(std::string name, std::string value, bool enabled = true) {
    std::string n = trim(std::move(name));
    if (enabled) {
      if (n.empty())
        return Result<Header>::fail({ErrorCode::Validation, "header name must not be empty", "header.name"});
      if (!isToken(n))
        return Result<Header>::fail({ErrorCode::Validation, "invalid header name: " + n, "header.name"});
    }
    return Result<Header>::ok(Header(std::move(n), std::move(value), enabled));
  }
  const std::string &name() const noexcept { return key_; }

private:
  Header(std::string n, std::string v, bool en) : KeyValueEntry(std::move(n), std::move(v), en) {}

  static std::string trim(std::string s) {
    auto ns = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
    s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
    return s;
  }
  // RFC 7230 token: visible ASCII excluding separators.
  static bool isToken(const std::string &s) {
    static const std::string seps = "()<>@,;:\\\"/[]?={} \t";
    for (unsigned char c : s) {
      if (c <= 0x20 || c >= 0x7f) return false;
      if (seps.find((char)c) != std::string::npos) return false;
    }
    return true;
  }
};

// Ordered list with the usual conveniences; order is preserved and duplicates allowed.
class HeaderList {
public:
  HeaderList() = default;
  explicit HeaderList(std::vector<Header> items) : items_(std::move(items)) {}

  const std::vector<Header> &items() const noexcept { return items_; }
  std::size_t size() const noexcept { return items_.size(); }
  bool empty() const noexcept { return items_.empty(); }

  std::vector<Header> enabledOnly() const {
    std::vector<Header> out;
    for (const auto &h : items_)
      if (h.enabled()) out.push_back(h);
    return out;
  }
  // Case-insensitive lookup of the first entry with the given name.
  std::optional<Header> find(const std::string &name) const {
    for (const auto &h : items_)
      if (iequals(h.name(), name)) return h;
    return std::nullopt;
  }

  bool operator==(const HeaderList &o) const { return items_ == o.items_; }
  bool operator!=(const HeaderList &o) const { return !(*this == o); }

private:
  static bool iequals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
  }
  std::vector<Header> items_;
};

} // namespace core::domain
