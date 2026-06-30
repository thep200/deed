// core/domain/auth/auth.hpp — Auth sum type (REFACTOR_SPEC §5.2).
// Auth is DATA only: applying it to a request (Base64 for Basic, the Authorization header, an api-key
// query param) is the sender's job (infra), never the domain's.
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
enum class ApiKeyIn { Header, Query };
struct AuthApiKey {
  std::string key;
  std::string value;
  ApiKeyIn in = ApiKeyIn::Header;
  bool operator==(const AuthApiKey &o) const {
    return key == o.key && value == o.value && in == o.in;
  }
};

class Auth {
public:
  using Variant = std::variant<AuthNone, AuthBasic, AuthBearer, AuthApiKey>;

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
  static Result<Auth> apiKey(std::string key, std::string value, ApiKeyIn in) {
    if (key.empty())
      return Result<Auth>::fail({ErrorCode::Validation, "api key name required", "auth.key"});
    return Result<Auth>::ok(Auth(AuthApiKey{std::move(key), std::move(value), in}));
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
