// core/src/infra/transport/grpc/native_grpc_sender.hpp — REFACTOR_SPEC P6 / Phase C1.
// gRPC sender on the DOMAIN port (core::domain::IRequestSender). It owns the grpc++ machinery directly
// (dynamic descriptors + GenericStub + CompletionQueue), emits domain ResponseEvents, and routes by the
// request's GrpcMethodType: Unary/ClientStreaming -> a unary-shaped EvCompleted; ServerStreaming/
// BidiStreaming -> EvMessage* then EvCompleted/EvFailed. Replaces the old core::GrpcSender +
// LegacySenderAdapter(Grpc) pair. Internally it still maps the domain payload to the legacy
// core::GrpcRequest struct (via the request bridge) so the proven descriptor/marshal code is reused.
#pragma once

#include <cstdint>

#include "core/domain/ports/driven/i_request_sender.hpp"

namespace core::infra {

// gRPC streaming ceilings (SPEC §9). Defaults here; the composition root overrides them from .env
// (STREAM_MAX_EVENTS / STREAM_MAX_BYTES_MB) via CoreApiClient::Config — Core never reads .env itself.
struct GrpcStreamLimits {
  std::uint64_t maxEvents = 100000;             // truncate the stream after this many events
  std::uint64_t maxBytes = 64ull * 1024 * 1024; // truncate after this many accumulated payload bytes
};

class NativeGrpcSender final : public domain::IRequestSender {
public:
  NativeGrpcSender() = default;
  explicit NativeGrpcSender(GrpcStreamLimits limits) : limits_(limits) {}

  bool supports(domain::RequestType t) const override { return t == domain::RequestType::Grpc; }

  // Mid-flight cancel rides the CALLER's token (core::linkCancel): reflection ctx TryCancel + the CQ pump.
  // No member token slot / close() override — this instance is shared by every concurrent gRPC send, so a
  // slot would cancel whichever call started last.
  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;

private:
  GrpcStreamLimits limits_; // stream ceilings (.env via composition root; default above)
};

} // namespace core::infra
