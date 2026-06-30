// core/domain/values/url.hpp — Url value object (REFACTOR_SPEC §5.1).
// Loose validation at the domain level: non-empty, and a placeholder-aware scheme check. Full resolution
// of {{vars}} happens later (IVariableResolver); domain only guards the obvious invariants.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/common/result.hpp"

namespace core::domain {

class Url {
public:
  // Loose: trims only. Emptiness is allowed so a DRAFT request (no URL typed yet) is a valid domain value;
  // "must be non-empty to send" is a send-time check, not a construction invariant. Scheme is NOT enforced
  // here (a URL may be `{{base}}/x`). Use createWithSchemes() when a context (WS) demands a scheme set.
  static Result<Url> create(std::string raw) { return Result<Url>::ok(Url(trim(std::move(raw)))); }

  bool empty() const noexcept { return raw_.empty(); }

  // Enforce that the scheme (when present and not a placeholder) is one of `allowed`. A URL beginning with
  // `{{` is accepted (resolved later). Used by WebSocketRequest (ws/wss) and similar.
  static Result<Url> createWithSchemes(std::string raw, const std::vector<std::string> &allowed) {
    auto r = create(std::move(raw));
    if (!r) return r;
    const std::string &s = r.value().scheme();
    if (s.empty() || r.value().startsWithPlaceholder()) return r; // can't judge yet
    if (std::find(allowed.begin(), allowed.end(), s) == allowed.end())
      return Result<Url>::fail({ErrorCode::Validation, "url scheme '" + s + "' not allowed", "url"});
    return r;
  }

  const std::string &raw() const noexcept { return raw_; }

  // The scheme part before "://", lowercased; empty when there is none (e.g. relative or placeholder URL).
  std::string scheme() const {
    auto pos = raw_.find("://");
    if (pos == std::string::npos) return {};
    std::string s = raw_.substr(0, pos);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
  }

  bool startsWithPlaceholder() const { return raw_.rfind("{{", 0) == 0; }

  bool operator==(const Url &o) const { return raw_ == o.raw_; }
  bool operator!=(const Url &o) const { return raw_ != o.raw_; }

private:
  explicit Url(std::string raw) : raw_(std::move(raw)) {}

  static std::string trim(std::string s) {
    auto notspace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
  }

  std::string raw_;
};

} // namespace core::domain
