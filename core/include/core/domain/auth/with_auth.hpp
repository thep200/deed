// core/domain/auth/with_auth.hpp — auth transforms over a RequestModel (pure STL, REFACTOR_SPEC §5.2).
// Used by the send saga AND the GraphQL introspection path to swap an AuthOAuth2 config for the
// materialized AuthBearer before any sender runs — senders only ever see none/basic/bearer.
#pragma once

#include <type_traits>
#include <utility>

#include "core/domain/auth/auth.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::domain {

// The request's OAuth2 config, if its payload carries one (Http/GraphQl/WebSocket). Else nullptr.
inline const AuthOAuth2 *oauth2Of(const RequestModel &m) {
  const AuthOAuth2 *out = nullptr;
  m.match([&](auto &&p) {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, HttpRequest> || std::is_same_v<T, GraphQlRequest> ||
                  std::is_same_v<T, WebSocketRequest>) {
      p.auth().match([&](auto &&a) {
        if constexpr (std::is_same_v<std::decay_t<decltype(a)>, AuthOAuth2>) out = &a;
      });
    }
  });
  return out;
}

// Rebuild the model with `auth` swapped in (Http/GraphQl/WebSocket); other payload types pass through
// unchanged. The Parts factories re-run their invariants, which the source model already satisfied.
inline RequestModel withAuth(const RequestModel &m, Auth auth) {
  RequestModel::Payload payload = m.payload();
  m.match([&](auto &&p) {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, HttpRequest>) {
      HttpRequest::Parts hp{p.method(),  p.url(),  p.pathVariables(), p.params(),
                            p.headers(), p.body(), std::move(auth)};
      payload = HttpRequest::create(std::move(hp)).take();
    } else if constexpr (std::is_same_v<T, GraphQlRequest>) {
      GraphQlRequest::Parts gp{p.url(),          p.op(),         p.headers(),
                               std::move(auth),  p.subTransport(), p.wsProtocol()};
      auto r = GraphQlRequest::create(std::move(gp));
      if (r.isOk()) payload = r.take();
    } else if constexpr (std::is_same_v<T, WebSocketRequest>) {
      WebSocketRequest::Parts wp{p.url(),         p.subprotocols(),  p.headers(),
                                 std::move(auth), p.onOpenSend(),    p.defaultSendKind()};
      auto r = WebSocketRequest::create(std::move(wp));
      if (r.isOk()) payload = r.take();
    }
  });
  return m.withPayload(std::move(payload));
}

} // namespace core::domain
