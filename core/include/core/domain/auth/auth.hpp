// core/domain/auth/auth.hpp — Auth sum type (REFACTOR_SPEC §5.2).
// Auth is DATA only: applying it to a request (Base64 for Basic, the Authorization header) is the
// sender's job (infra), never the domain's. Custom auth headers/params (former "apikey" type) are just
// entries in the Headers/Query tab — no dedicated alternative.
#pragma once

#include <string>
#include <utility>
#include <variant>

#include "core/domain/common/result.hpp"

namespace core::domain {

struct AuthNone {
  bool operator==(const AuthNone &) const { return true; }
};
struct AuthBasic {
  std::string username;
  std::string password;
  bool operator==(const AuthBasic &o) const {
    return username == o.username && password == o.password;
  }
};
struct AuthBearer {
  std::string token;
  bool operator==(const AuthBearer &o) const { return token == o.token; }
};

enum class OAuth2Grant { ClientCredentials, Password };
// How the client authenticates at the token endpoint: Basic header (RFC 6749 §2.3.1 default) or
// client_id/client_secret as form-body params (what some IdPs require).
enum class OAuth2ClientAuth { Header, Body };
// OAuth2 CONFIG only — the fetched access token / expiry live in the infra token provider's cache,
// never in the domain. Every field is a plain string so {{var}} may sit anywhere (incl. the secret).
struct AuthOAuth2 {
  std::string tokenUrl;
  std::string clientId;
  std::string clientSecret; // optional (public clients)
  std::string scope;        // optional; empty -> param omitted
  std::string username;     // Password grant only
  std::string password;     // Password grant only
  OAuth2Grant grant = OAuth2Grant::ClientCredentials;
  OAuth2ClientAuth clientAuth = OAuth2ClientAuth::Header;
  bool operator==(const AuthOAuth2 &o) const {
    return tokenUrl == o.tokenUrl && clientId == o.clientId && clientSecret == o.clientSecret &&
           scope == o.scope && username == o.username && password == o.password && grant == o.grant &&
           clientAuth == o.clientAuth;
  }
};

class Auth {
public:
  using Variant = std::variant<AuthNone, AuthBasic, AuthBearer, AuthOAuth2>;

  static Auth none() { return Auth(AuthNone{}); }

  static Result<Auth> basic(std::string user, std::string pass) {
    if (user.empty())
      return Result<Auth>::fail({ErrorCode::Validation, "basic auth username required", "auth.username"});
    return Result<Auth>::ok(Auth(AuthBasic{std::move(user), std::move(pass)}));
  }
  static Result<Auth> bearer(std::string token) {
    if (token.empty())
      return Result<Auth>::fail({ErrorCode::Validation, "bearer token required", "auth.token"});
    return Result<Auth>::ok(Auth(AuthBearer{std::move(token)}));
  }
  static Result<Auth> oauth2(AuthOAuth2 o) {
    if (o.tokenUrl.empty())
      return Result<Auth>::fail({ErrorCode::Validation, "oauth2 tokenUrl required", "auth.tokenUrl"});
    if (o.clientId.empty())
      return Result<Auth>::fail({ErrorCode::Validation, "oauth2 clientId required", "auth.clientId"});
    if (o.grant == OAuth2Grant::Password && (o.username.empty() || o.password.empty()))
      return Result<Auth>::fail(
          {ErrorCode::Validation, "oauth2 password grant needs username and password", "auth.username"});
    return Result<Auth>::ok(Auth(AuthOAuth2{std::move(o)}));
  }
  // Exhaustive visit (compile-time enforced by the overload set). No RTTI.
  template <class V> decltype(auto) match(V &&v) const { return std::visit(std::forward<V>(v), data_); }

  bool isNone() const { return std::holds_alternative<AuthNone>(data_); }

  bool operator==(const Auth &o) const { return data_ == o.data_; }
  bool operator!=(const Auth &o) const { return !(data_ == o.data_); }

private:
  explicit Auth(Variant v) : data_(std::move(v)) {}
  Variant data_;
};

} // namespace core::domain
