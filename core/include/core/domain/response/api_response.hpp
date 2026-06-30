// core/domain/response/api_response.hpp — unary (or final aggregated) result (REFACTOR_SPEC §5.9).
#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace core::domain {

struct ResponseHeader {
  std::string name;
  std::string value;
  bool operator==(const ResponseHeader &o) const { return name == o.name && value == o.value; }
};

struct Cookie {
  std::string name;
  std::string value;
  std::string domain;
  std::string path;
  std::string expires;
  bool operator==(const Cookie &o) const {
    return name == o.name && value == o.value && domain == o.domain && path == o.path &&
           expires == o.expires;
  }
};

struct ApiResponse {
  int statusCode = 0; // HTTP status; gRPC status mapped onto it
  std::vector<ResponseHeader> headers;
  std::vector<Cookie> cookies;
  std::string body; // raw text; the UI pretty-prints
  std::chrono::milliseconds elapsed{0};

  bool operator==(const ApiResponse &o) const {
    return statusCode == o.statusCode && headers == o.headers && cookies == o.cookies &&
           body == o.body && elapsed == o.elapsed;
  }
};

} // namespace core::domain
