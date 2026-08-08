#pragma once

#include <cstddef>
#include <string>

namespace core {

// Order MUST match RequestModel::Payload's variant order (type() == variant index) — new types
// APPEND at the end. request_traits.hpp static_asserts this alignment.
enum class RequestType { Http, Grpc, GraphQl, WebSocket, Kafka, Soap, Ldap };

inline constexpr std::size_t kRequestTypeCount = 7;

// Wire token: the JSON "type" value AND the per-type payload block key. Index = enum value.
inline constexpr const char *kRequestTypeTokens[kRequestTypeCount] = {
    "http", "grpc", "graphql", "ws", "kafka", "soap", "ldap"};

inline std::string toString(RequestType t) {
  auto i = static_cast<std::size_t>(t);
  return kRequestTypeTokens[i < kRequestTypeCount ? i : 0];
}

inline bool parseRequestType(const std::string &s, RequestType &out) {
  for (std::size_t i = 0; i < kRequestTypeCount; ++i)
    if (s == kRequestTypeTokens[i]) {
      out = static_cast<RequestType>(i);
      return true;
    }
  return false;
}

} // namespace core
