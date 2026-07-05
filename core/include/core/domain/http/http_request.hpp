// core/domain/http/http_request.hpp — HttpRequest aggregate payload (REFACTOR_SPEC §5.4).
#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

#include "core/domain/auth/auth.hpp"
#include "core/domain/body/body.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/values/header.hpp"
#include "core/domain/values/http_method.hpp"
#include "core/domain/values/path_variable.hpp"
#include "core/domain/values/query_param.hpp"
#include "core/domain/values/url.hpp"

namespace core::domain {

class HttpRequest {
public:
  struct Parts {
    HttpMethod method = HttpMethod::Get;
    Url url;
    PathVariableList pathVariables;
    QueryParamList params;
    HeaderList headers;
    Body body = Body::none();
    Auth auth = Auth::none();
  };

  // The url<->path-variable consistency is a use-case-layer concern (a missing binding is not rejected
  // here) — the URL may legitimately contain `{{vars}}` or `:seg` resolved later. The factory therefore
  // only assembles; structural URL/body checks happen in the use-case layer.
  static Result<HttpRequest> create(Parts p) {
    return Result<HttpRequest>::ok(HttpRequest(std::move(p)));
  }

  HttpMethod method() const noexcept { return p_.method; }
  const Url &url() const noexcept { return p_.url; }
  const PathVariableList &pathVariables() const noexcept { return p_.pathVariables; }
  const QueryParamList &params() const noexcept { return p_.params; }
  const HeaderList &headers() const noexcept { return p_.headers; }
  const Body &body() const noexcept { return p_.body; }
  const Auth &auth() const noexcept { return p_.auth; }

  bool operator==(const HttpRequest &o) const {
    return p_.method == o.p_.method && p_.url == o.p_.url && p_.pathVariables == o.p_.pathVariables &&
           p_.params == o.p_.params && p_.headers == o.p_.headers && p_.body == o.p_.body &&
           p_.auth == o.p_.auth;
  }
  bool operator!=(const HttpRequest &o) const { return !(*this == o); }

private:
  explicit HttpRequest(Parts p) : p_(std::move(p)) {}
  Parts p_;
};

// True when an enabled `Accept` header contains `text/event-stream` — the signal that forces the SSE path
// (the model has no separate streamMode field). Case-insensitive, compared in place (no allocations).
inline bool acceptsEventStream(const HttpRequest &h) {
  constexpr const char *kAccept = "accept";
  constexpr const char *kEventStream = "text/event-stream";
  constexpr std::size_t kAcceptLen = 6, kEventStreamLen = 17;
  auto low = [](char c) { return (char)std::tolower((unsigned char)c); };
  for (const auto &hd : h.headers().items()) {
    if (!hd.enabled()) continue;
    const std::string &n = hd.name();
    if (n.size() != kAcceptLen) continue;
    bool isAccept = true;
    for (std::size_t i = 0; isAccept && i < kAcceptLen; ++i) isAccept = low(n[i]) == kAccept[i];
    if (!isAccept) continue;
    const std::string &v = hd.value();
    for (std::size_t i = 0; i + kEventStreamLen <= v.size(); ++i) {
      bool match = true;
      for (std::size_t j = 0; match && j < kEventStreamLen; ++j) match = low(v[i + j]) == kEventStream[j];
      if (match) return true;
    }
  }
  return false;
}

} // namespace core::domain
