// core/domain/request/request_type.hpp — the request protocol classification enum, split out of request_model.hpp so it
// SURVIVES the legacy types.hpp removal (REFACTOR_SPEC P6). It is a transport-free view/classification enum
// used by the UI, the lazy-tree TreeNode (env_config.hpp), and the request serializers. Tiny + dependency-free.
#pragma once

#include <string>

namespace core {

// Protocol classification (matches the "type" field in the request file).
// Order MUST match RequestModel::Payload's variant order (type() == variant index).
enum class RequestType { Http, Grpc, GraphQl, WebSocket, Kafka };

inline std::string toString(RequestType t) {
  switch (t) {
  case RequestType::Grpc: return "grpc";
  case RequestType::WebSocket: return "ws";
  case RequestType::GraphQl: return "graphql";
  case RequestType::Kafka: return "kafka";
  default: return "http";
  }
}

inline bool parseRequestType(const std::string &s, RequestType &out) {
  if (s == "http") { out = RequestType::Http; return true; }
  if (s == "grpc") { out = RequestType::Grpc; return true; }
  if (s == "ws") { out = RequestType::WebSocket; return true; }
  if (s == "graphql") { out = RequestType::GraphQl; return true; }
  if (s == "kafka") { out = RequestType::Kafka; return true; }
  return false;
}

} // namespace core
