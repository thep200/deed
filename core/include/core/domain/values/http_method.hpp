// core/domain/values/http_method.hpp — HttpMethod enum + parse/format (REFACTOR_SPEC §5.1).
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include "core/domain/common/result.hpp"

namespace core::domain {

enum class HttpMethod { Get, Post, Put, Patch, Delete, Head, Options };

inline std::string toString(HttpMethod m) {
  switch (m) {
  case HttpMethod::Get: return "GET";
  case HttpMethod::Post: return "POST";
  case HttpMethod::Put: return "PUT";
  case HttpMethod::Patch: return "PATCH";
  case HttpMethod::Delete: return "DELETE";
  case HttpMethod::Head: return "HEAD";
  case HttpMethod::Options: return "OPTIONS";
  }
  return "GET";
}

// Case-insensitive parse. Unknown verb -> Validation error (field "method").
inline Result<HttpMethod> parseHttpMethod(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::toupper(c); });
  if (s == "GET") return Result<HttpMethod>::ok(HttpMethod::Get);
  if (s == "POST") return Result<HttpMethod>::ok(HttpMethod::Post);
  if (s == "PUT") return Result<HttpMethod>::ok(HttpMethod::Put);
  if (s == "PATCH") return Result<HttpMethod>::ok(HttpMethod::Patch);
  if (s == "DELETE") return Result<HttpMethod>::ok(HttpMethod::Delete);
  if (s == "HEAD") return Result<HttpMethod>::ok(HttpMethod::Head);
  if (s == "OPTIONS") return Result<HttpMethod>::ok(HttpMethod::Options);
  return Result<HttpMethod>::fail({ErrorCode::Validation, "unknown http method: " + s, "method"});
}

} // namespace core::domain
