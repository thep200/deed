// core/domain/grpc/grpc_method.hpp — gRPC method type enum + descriptor (REFACTOR_SPEC §5.5).
#pragma once

#include <string>

#include "core/domain/common/result.hpp"

namespace core::domain {

enum class GrpcMethodType { Unary, ServerStreaming, ClientStreaming, BidiStreaming };

inline std::string toString(GrpcMethodType t) {
  switch (t) {
  case GrpcMethodType::Unary: return "unary";
  case GrpcMethodType::ServerStreaming: return "server_streaming";
  case GrpcMethodType::ClientStreaming: return "client_streaming";
  case GrpcMethodType::BidiStreaming: return "bidi_streaming";
  }
  return "unary";
}

inline Result<GrpcMethodType> parseGrpcMethodType(const std::string &s) {
  if (s == "unary") return Result<GrpcMethodType>::ok(GrpcMethodType::Unary);
  if (s == "server_streaming") return Result<GrpcMethodType>::ok(GrpcMethodType::ServerStreaming);
  if (s == "client_streaming") return Result<GrpcMethodType>::ok(GrpcMethodType::ClientStreaming);
  if (s == "bidi_streaming") return Result<GrpcMethodType>::ok(GrpcMethodType::BidiStreaming);
  return Result<GrpcMethodType>::fail({ErrorCode::Validation, "unknown grpc methodType: " + s, "methodType"});
}

// One discoverable RPC (returned by IApiClient::listGrpcMethods for the service/method dropdown).
struct GrpcMethodDescriptor {
  std::string service; // full pkg.Service
  std::string method;  // RPC name
  GrpcMethodType type = GrpcMethodType::Unary;
  bool operator==(const GrpcMethodDescriptor &o) const {
    return service == o.service && method == o.method && type == o.type;
  }
};

} // namespace core::domain
