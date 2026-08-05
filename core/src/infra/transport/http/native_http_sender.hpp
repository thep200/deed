// core/src/infra/transport/http/native_http_sender.hpp — REFACTOR_SPEC P6 / Phase C2: HTTP sender that consumes
// the DOMAIN model directly via cpr — both unary AND SSE are native now (no legacy core::HttpSender). SSE
// streams the response, feeds each chunk to the pure SseParser, and emits EvMessage* then a terminal event.
#pragma once

#include "core/domain/ports/driven/i_request_sender.hpp"
#include "infra/transport/shared/cancel_token.hpp" // cancel primitive (cpr progress callback + write-callback abort)

namespace core::infra {

// Stateless across calls: the abort path hangs off the CALLER's token (core::linkCancel), not a member
// slot — one instance serves every concurrent HTTP send, so a member slot would route Cancel to the wrong
// request. No close() override for the same reason; cancel is the token's job.
class NativeHttpSender final : public domain::IRequestSender {
public:
  bool supports(domain::RequestType t) const override { return t == domain::RequestType::Http; }
  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;
};

} // namespace core::infra
