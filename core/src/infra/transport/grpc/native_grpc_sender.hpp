// core/src/infra/transport/grpc/native_grpc_sender.hpp — REFACTOR_SPEC P6 / Phase C1.
// gRPC sender on the DOMAIN port (core::domain::IRequestSender). It owns the grpc++ machinery directly
// (dynamic descriptors + GenericStub + CompletionQueue), emits domain ResponseEvents, and routes by the
// request's GrpcMethodType: Unary/ClientStreaming -> a unary-shaped EvCompleted; ServerStreaming/
// BidiStreaming -> EvMessage* then EvCompleted/EvFailed. Replaces the old core::GrpcSender +
// LegacySenderAdapter(Grpc) pair. Internally it still maps the domain payload to the legacy
// core::GrpcRequest struct (via the request bridge) so the proven descriptor/marshal code is reused.
#pragma once

#include <memory>
#include <mutex>

#include "core/domain/ports/driven/i_request_sender.hpp"

namespace core {
class CancelToken;
}

namespace core::infra {

class NativeGrpcSender final : public domain::IRequestSender {
public:
  bool supports(domain::RequestType t) const override { return t == domain::RequestType::Grpc; }

  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;
  // Mid-flight cancel: saga.cancel()/closeStream() calls this from another thread -> cancel the in-flight
  // token so the blocking unary/stream stops (gRPC TryCancel via the CQ pump).
  domain::Status close(int code, std::string reason) override;

private:
  std::mutex mu_;
  std::shared_ptr<core::CancelToken> token_; // active call's cancel token (one in-flight per sender)
};

} // namespace core::infra
