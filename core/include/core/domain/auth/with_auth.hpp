// AuthOAuth2 is swapped for the materialized AuthBearer before any sender runs — senders only ever see none/basic/bearer.
#pragma once

#include <type_traits>
#include <utility>

#include "core/domain/auth/auth.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::domain {

template <class T, class = void> struct HasAuth : std::false_type {};
template <class T>
struct HasAuth<T, std::void_t<decltype(std::declval<const T &>().auth())>> : std::true_type {};

inline const AuthOAuth2 *oauth2Of(const RequestModel &m) {
  const AuthOAuth2 *out = nullptr;
  m.match([&](const auto &p) {
    using T = std::decay_t<decltype(p)>;
    if constexpr (HasAuth<T>::value) {
      p.auth().match([&](auto &&a) {
        if constexpr (std::is_same_v<std::decay_t<decltype(a)>, AuthOAuth2>) out = &a;
      });
    }
  });
  return out;
}

// HTTP asserts via take() (source model already valid); the others keep the old payload on failure.
inline RequestModel::Payload withAuthTyped(const HttpRequest &p, Auth auth) {
  HttpRequest::Parts hp{p.method(),  p.url(),  p.pathVariables(), p.params(),
                        p.headers(), p.body(), std::move(auth)};
  return HttpRequest::create(std::move(hp)).take();
}
inline RequestModel::Payload withAuthTyped(const GraphQlRequest &p, Auth auth) {
  GraphQlRequest::Parts gp{p.url(),         p.op(),           p.headers(),
                           std::move(auth), p.subTransport(), p.wsProtocol()};
  auto r = GraphQlRequest::create(std::move(gp));
  return r.isOk() ? RequestModel::Payload(r.take()) : RequestModel::Payload(p);
}
inline RequestModel::Payload withAuthTyped(const WebSocketRequest &p, Auth auth) {
  WebSocketRequest::Parts wp{p.url(),         p.subprotocols(), p.headers(),
                             std::move(auth), p.onOpenSend(),   p.defaultSendKind()};
  auto r = WebSocketRequest::create(std::move(wp));
  return r.isOk() ? RequestModel::Payload(r.take()) : RequestModel::Payload(p);
}
inline RequestModel::Payload withAuthTyped(const SoapRequest &p, Auth auth) {
  SoapRequest::Parts sp{p.url(), p.action(), p.version(), p.envelope(), p.headers(),
                        std::move(auth)};
  auto r = SoapRequest::create(std::move(sp));
  return r.isOk() ? RequestModel::Payload(r.take()) : RequestModel::Payload(p);
}
// Auth-free payloads pass through; the static_assert stops a new auth-carrying type from silently skipping the swap.
template <class T> RequestModel::Payload withAuthTyped(const T &p, Auth) {
  static_assert(!HasAuth<T>::value, "payload has auth() but no withAuthTyped overload");
  return p;
}

inline RequestModel withAuth(const RequestModel &m, Auth auth) {
  return m.withPayload(
      m.match([&](const auto &p) { return withAuthTyped(p, std::move(auth)); }));
}

} // namespace core::domain
