#pragma once

#include "core/domain/graphql/gql_operation.hpp"
#include "core/domain/http/http_request.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/response/interaction.hpp"

namespace core::domain {

// Dispatch is by overload resolution — a new request type without its overload is a compile error.
inline InteractionKind interactionOfTyped(const HttpRequest &p) {
  // SSE = an enabled Accept: text/event-stream header (the native HTTP sender streams on this signal).
  return acceptsEventStream(p) ? InteractionKind::ServerStream : InteractionKind::Unary;
}
inline InteractionKind interactionOfTyped(const GrpcRequest &p) {
  switch (p.methodType()) {
  case GrpcMethodType::ServerStreaming: return InteractionKind::ServerStream;
  case GrpcMethodType::ClientStreaming: return InteractionKind::ClientStream;
  case GrpcMethodType::BidiStreaming: return InteractionKind::BiDi;
  default: return InteractionKind::Unary;
  }
}
inline InteractionKind interactionOfTyped(const GraphQlRequest &g) {
  return effectiveOperation(g) == GqlOperationType::Subscription ? InteractionKind::ServerStream
                                                                 : InteractionKind::Unary;
}
inline InteractionKind interactionOfTyped(const WebSocketRequest &) {
  return InteractionKind::Duplex;
}
inline InteractionKind interactionOfTyped(const KafkaRequest &k) {
  // Producer = unary (one delivery report); Consumer = inbound-only records, same shape as gRPC ServerStream.
  return k.kind() == KafkaClientKind::Consumer ? InteractionKind::ServerStream
                                               : InteractionKind::Unary;
}
inline InteractionKind interactionOfTyped(const SoapRequest &) { return InteractionKind::Unary; }
inline InteractionKind interactionOfTyped(const LdapRequest &) { return InteractionKind::Unary; }

inline InteractionKind interactionOf(const RequestModel &m) {
  return m.match([](const auto &p) { return interactionOfTyped(p); });
}

} // namespace core::domain
