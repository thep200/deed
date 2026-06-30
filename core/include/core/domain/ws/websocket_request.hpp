// core/domain/ws/websocket_request.hpp — WebSocketRequest aggregate payload (REFACTOR_SPEC §5.7).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/domain/auth/auth.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/values/header.hpp"
#include "core/domain/values/url.hpp"
#include "core/domain/ws/ws_message.hpp"

namespace core::domain {

class WebSocketRequest {
public:
  struct Parts {
    Url url; // scheme must be ws/wss (validated via Url::createWithSchemes before building Parts)
    std::vector<std::string> subprotocols;
    HeaderList headers;
    Auth auth = Auth::none();
    std::vector<WsMessage> onOpenSend; // sent immediately after the socket opens
    WsSendKind defaultSendKind = WsSendKind::Text;
  };

  // Invariant: URL scheme must be ws/wss (or a placeholder). Re-checks here so callers can't bypass it.
  static Result<WebSocketRequest> create(Parts p) {
    const std::string s = p.url.scheme();
    if (!s.empty() && !p.url.startsWithPlaceholder() && s != "ws" && s != "wss")
      return Result<WebSocketRequest>::fail(
          {ErrorCode::Validation, "websocket url must use ws/wss: " + s, "ws.url"});
    return Result<WebSocketRequest>::ok(WebSocketRequest(std::move(p)));
  }

  const Url &url() const noexcept { return p_.url; }
  const std::vector<std::string> &subprotocols() const noexcept { return p_.subprotocols; }
  const HeaderList &headers() const noexcept { return p_.headers; }
  const Auth &auth() const noexcept { return p_.auth; }
  const std::vector<WsMessage> &onOpenSend() const noexcept { return p_.onOpenSend; }
  WsSendKind defaultSendKind() const noexcept { return p_.defaultSendKind; }

  bool operator==(const WebSocketRequest &o) const {
    return p_.url == o.p_.url && p_.subprotocols == o.p_.subprotocols && p_.headers == o.p_.headers &&
           p_.auth == o.p_.auth && p_.onOpenSend == o.p_.onOpenSend &&
           p_.defaultSendKind == o.p_.defaultSendKind;
  }
  bool operator!=(const WebSocketRequest &o) const { return !(*this == o); }

private:
  explicit WebSocketRequest(Parts p) : p_(std::move(p)) {}
  Parts p_;
};

} // namespace core::domain
