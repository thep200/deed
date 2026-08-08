#pragma once

#include <cstdint>

#include "infra/transport/typed_sender.hpp"

namespace core::infra {

// Defaults here; the composition root overrides them from .env — Core never reads .env itself.
struct GrpcStreamLimits {
  std::uint64_t maxEvents = 100000;             // truncate the stream after this many events
  std::uint64_t maxBytes = 64ull * 1024 * 1024; // truncate after this many accumulated payload bytes
};

class NativeGrpcSender final : public TypedSender<domain::GrpcRequest> {
public:
  NativeGrpcSender() = default;
  explicit NativeGrpcSender(GrpcStreamLimits limits) : limits_(limits) {}

protected:
  // Cancel rides the caller's token (linkCancel): reflection TryCancel + the CQ pump. No member token
  // slot / close() override — this instance is shared by every concurrent gRPC send.
  domain::Status executeTyped(const domain::RequestModel &resolved, const domain::GrpcRequest &grpc,
                              domain::IResponseSink &sink,
                              const domain::ICancellationToken &cancel) override;
  const char *mismatchMessage() const override { return "NativeGrpcSender: not a gRPC request"; }

private:
  GrpcStreamLimits limits_; // stream ceilings (.env via composition root; default above)
};

} // namespace core::infra
