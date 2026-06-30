// core/domain/ports/i_request_sender.hpp — driven port: one impl per transport (REFACTOR_SPEC §6.3).
// The sender receives the DOMAIN RequestModel and emits ResponseEvents through the sink; it NEVER sees JSON
// (it translates to cpr/grpc/ws internally). Interactive streaming uses push/halfClose/close.
#pragma once

#include <string>

#include "core/domain/common/result.hpp"
#include "core/domain/ports/i_cancellation_token.hpp"
#include "core/domain/ports/i_response_sink.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/ws/ws_message.hpp"

namespace core::domain {

class IRequestSender {
public:
  virtual ~IRequestSender() = default;

  virtual bool supports(RequestType) const = 0;

  // Execute a resolved request. Emits events via `sink`; returns when setup is established (unary: after
  // the terminal event has been emitted). Must honor `cancel` cooperatively.
  virtual Status execute(const RequestModel &resolved, IResponseSink &sink,
                         const ICancellationToken &cancel) = 0;

  // Interactive streaming hooks (WS / gRPC client-stream); default = Unsupported.
  virtual Status push(WsMessage) { return Status::fail({ErrorCode::Unsupported, "push"}); }
  virtual Status halfClose() { return Status::fail({ErrorCode::Unsupported, "halfClose"}); }
  virtual Status close(int, std::string) { return Status::fail({ErrorCode::Unsupported, "close"}); }
};

} // namespace core::domain
