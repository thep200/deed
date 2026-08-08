#pragma once

#include <string>

#include "core/domain/common/result.hpp"
#include "core/domain/ports/driven/i_cancellation_token.hpp"
#include "core/domain/ports/driven/i_response_sink.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/ws/ws_message.hpp"

namespace core::domain {

class IRequestSender {
public:
  virtual ~IRequestSender() = default;

  virtual bool supports(RequestType) const = 0;

  // Emits events via `sink`; returns once setup is established (unary: after the terminal event). Must honor `cancel`.
  virtual Status execute(const RequestModel &resolved, IResponseSink &sink,
                         const ICancellationToken &cancel) = 0;

  // Interactive streaming hooks (WS / gRPC client-stream); default = Unsupported.
  virtual Status push(WsMessage) { return Status::fail({ErrorCode::Unsupported, "push"}); }
  virtual Status halfClose() { return Status::fail({ErrorCode::Unsupported, "halfClose"}); }
  virtual Status close(int, std::string) { return Status::fail({ErrorCode::Unsupported, "close"}); }
};

} // namespace core::domain
