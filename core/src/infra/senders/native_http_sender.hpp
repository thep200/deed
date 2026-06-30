// core/src/infra/senders/native_http_sender.hpp — REFACTOR_SPEC P6 / Phase C2: HTTP sender that consumes
// the DOMAIN model directly via cpr — both unary AND SSE are native now (no legacy core::HttpSender). SSE
// streams the response, feeds each chunk to the pure SseParser, and emits EvMessage* then a terminal event.
#pragma once

#include <memory>
#include <mutex>

#include "core/domain/ports/i_request_sender.hpp"
#include "core/sending/cancel_token.hpp" // cancel primitive (cpr progress callback + write-callback abort)

namespace core::infra {

class NativeHttpSender final : public domain::IRequestSender {
public:
  bool supports(domain::RequestType t) const override { return t == domain::RequestType::Http; }
  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;
  domain::Status close(int code, std::string reason) override; // mid-flight cancel

private:
  std::mutex mu_;
  std::shared_ptr<core::CancelToken> token_; // active request's cancel token
};

} // namespace core::infra
