#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/values/url.hpp"

namespace core::domain {

// RFC 4511 search scope.
enum class LdapScope { Base, One, Sub };

inline std::string toString(LdapScope s) {
  switch (s) {
  case LdapScope::Base: return "base";
  case LdapScope::One: return "one";
  default: return "sub";
  }
}

inline bool parseLdapScope(const std::string &s, LdapScope &out) {
  if (s == "base") { out = LdapScope::Base; return true; }
  if (s == "one") { out = LdapScope::One; return true; }
  if (s == "sub") { out = LdapScope::Sub; return true; }
  return false;
}

// RFC 4515 filter-value escaping: \ * ( ) NUL.
inline std::string escapeLdapFilterValue(const std::string &v) {
  std::string out;
  out.reserve(v.size());
  for (char c : v) {
    switch (c) {
    case '\\': out += "\\5c"; break;
    case '*': out += "\\2a"; break;
    case '(': out += "\\28"; break;
    case ')': out += "\\29"; break;
    case '\0': out += "\\00"; break;
    default: out += c;
    }
  }
  return out;
}

// Effective filter sent to the server: empty filter -> match-all; group set -> AND a memberOf clause.
inline std::string composeLdapFilter(const std::string &filter, const std::string &group) {
  std::string base = filter.empty() ? std::string("(objectClass=*)") : filter;
  if (base.front() != '(') base = "(" + base + ")"; // bare "uid=bob" -> "(uid=bob)"
  if (group.empty()) return base;
  return "(&" + base + "(memberOf=" + escapeLdapFilterValue(group) + "))";
}

class LdapRequest {
public:
  struct Parts {
    Url url;                             // ldap:// | ldaps://; empty OK on a draft (send-time concern)
    bool startTls = false;               // ldap:// only — upgrade the plain connection (RFC 4513)
    std::string bindDn;                  // empty = anonymous bind
    std::string bindPassword;
    std::string baseDn;
    LdapScope scope = LdapScope::Sub;
    std::string filter = "(objectClass=*)";
    std::vector<std::string> attributes; // empty = all attributes
    std::string group;                   // empty = skip; group DN, ANDed into the filter via memberOf
    std::string testPassword;            // empty = skip; second bind with the found entry's DN
    int sizeLimit = 100;                 // 0 = server default/no client limit
    int timeLimit = 10;                  // seconds, server-side; 0 = no limit
    // RFC 2696 paged results; 0 = don't send the control. Sent NON-critical so non-paging servers answer in one go.
    int pageSize = 500;
  };

  // Lenient like other drafts, but the scheme (when typed) must be ldap/ldaps and limits non-negative.
  static Result<LdapRequest> create(Parts p) {
    const std::string s = p.url.scheme();
    if (!s.empty() && !p.url.startsWithPlaceholder() && s != "ldap" && s != "ldaps")
      return Result<LdapRequest>::fail(
          {ErrorCode::Validation, "url scheme '" + s + "' not allowed (ldap/ldaps)", "url"});
    if (p.sizeLimit < 0)
      return Result<LdapRequest>::fail({ErrorCode::Validation, "sizeLimit must be >= 0", "sizeLimit"});
    if (p.timeLimit < 0)
      return Result<LdapRequest>::fail({ErrorCode::Validation, "timeLimit must be >= 0", "timeLimit"});
    if (p.pageSize < 0)
      return Result<LdapRequest>::fail({ErrorCode::Validation, "pageSize must be >= 0", "pageSize"});
    if (p.startTls && s == "ldaps")
      return Result<LdapRequest>::fail(
          {ErrorCode::Validation, "startTls is for ldap:// only (ldaps is already TLS)", "startTls"});
    return Result<LdapRequest>::ok(LdapRequest(std::move(p)));
  }

  const Url &url() const noexcept { return p_.url; }
  bool startTls() const noexcept { return p_.startTls; }
  const std::string &bindDn() const noexcept { return p_.bindDn; }
  const std::string &bindPassword() const noexcept { return p_.bindPassword; }
  const std::string &baseDn() const noexcept { return p_.baseDn; }
  LdapScope scope() const noexcept { return p_.scope; }
  const std::string &filter() const noexcept { return p_.filter; }
  const std::vector<std::string> &attributes() const noexcept { return p_.attributes; }
  const std::string &group() const noexcept { return p_.group; }
  const std::string &testPassword() const noexcept { return p_.testPassword; }
  int sizeLimit() const noexcept { return p_.sizeLimit; }
  int timeLimit() const noexcept { return p_.timeLimit; }
  int pageSize() const noexcept { return p_.pageSize; }

  bool operator==(const LdapRequest &o) const {
    return p_.url == o.p_.url && p_.startTls == o.p_.startTls && p_.bindDn == o.p_.bindDn &&
           p_.bindPassword == o.p_.bindPassword && p_.baseDn == o.p_.baseDn && p_.scope == o.p_.scope &&
           p_.filter == o.p_.filter && p_.attributes == o.p_.attributes && p_.group == o.p_.group &&
           p_.testPassword == o.p_.testPassword && p_.sizeLimit == o.p_.sizeLimit &&
           p_.timeLimit == o.p_.timeLimit && p_.pageSize == o.p_.pageSize;
  }
  bool operator!=(const LdapRequest &o) const { return !(*this == o); }

private:
  explicit LdapRequest(Parts p) : p_(std::move(p)) {}
  Parts p_;
};

} // namespace core::domain
