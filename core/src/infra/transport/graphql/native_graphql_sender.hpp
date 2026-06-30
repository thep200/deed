// core/src/infra/transport/graphql/native_graphql_sender.hpp — REFACTOR_SPEC P6 / Phase C3 (GraphQL transport, native).
// query/mutation -> NATIVE HTTP (gql::buildHttpModel repackaging + NativeHttpSender). subscription -> NATIVE
// WebSocket: drives GraphQlWsProtocol (graphql-transport-ws / graphql-ws) directly over the ws_sender pump
// and emits domain ResponseEvents. No legacy GraphQlSender / LegacySenderAdapter anymore.
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "core/domain/ports/driven/i_request_sender.hpp"
#include "infra/transport/http/native_http_sender.hpp"

namespace core {
struct WsSession;
class CancelToken;
} // namespace core

namespace core::infra {

class NativeGraphQlSender final : public domain::IRequestSender {
public:
  bool supports(domain::RequestType t) const override { return t == domain::RequestType::GraphQl; }
  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;
  domain::Status close(int code, std::string reason) override; // mid-flight cancel (HTTP or subscription)

private:
  domain::Status runSubscription(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                                 const domain::ICancellationToken &cancel);

  NativeHttpSender http_; // query/mutation over native HTTP (cpr on domain types)
  std::mutex mu_;         // guards the in-flight subscription session/token (set by execute, read by close)
  std::shared_ptr<core::WsSession> wsSession_;
  std::shared_ptr<core::CancelToken> wsToken_;
};

} // namespace core::infra
