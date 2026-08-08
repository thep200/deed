#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "infra/transport/http/native_http_sender.hpp"
#include "infra/transport/typed_sender.hpp"

namespace core {
struct WsSession;
class CancelToken;
} // namespace core

namespace core::infra {

class NativeGraphQlSender final : public TypedSender<domain::GraphQlRequest> {
public:
  domain::Status close(int code, std::string reason) override; // mid-flight cancel (HTTP or subscription)

protected:
  domain::Status executeTyped(const domain::RequestModel &resolved,
                              const domain::GraphQlRequest &gql, domain::IResponseSink &sink,
                              const domain::ICancellationToken &cancel) override;

private:
  domain::Status runSubscription(const domain::RequestModel &resolved,
                                 const domain::GraphQlRequest &gql, domain::IResponseSink &sink,
                                 const domain::ICancellationToken &cancel);

  NativeHttpSender http_; // query/mutation over native HTTP (cpr on domain types)
  std::mutex mu_;         // guards the in-flight subscription session/token (set by execute, read by close)
  std::shared_ptr<core::WsSession> wsSession_;
  std::shared_ptr<core::CancelToken> wsToken_;
};

} // namespace core::infra
