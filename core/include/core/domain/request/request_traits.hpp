#pragma once

#include <utility>
#include <variant>
#include <vector>

#include "core/domain/request/request_model.hpp"

namespace core::domain {

// One specialization per Payload alternative; the asserts below make a missing/misaligned specialization a compile error.
template <class T> struct RequestTraits;

template <class T> struct TypeTag { using type = T; };

template <> struct RequestTraits<HttpRequest> {
  static constexpr RequestType type = RequestType::Http;
  static RequestModel::Payload makeDefault() {
    // common headers as OFF-by-default hints (User-Agent on)
    std::vector<Header> hdrs;
    hdrs.push_back(Header::create("Content-Type", "application/json", false).take());
    hdrs.push_back(Header::create("Accept", "*/*", false).take());
    hdrs.push_back(Header::create("User-Agent", "deed", true).take());
    hdrs.push_back(Header::create("Accept-Encoding", "gzip, deflate, br", false).take());
    hdrs.push_back(Header::create("Connection", "keep-alive", false).take());
    return HttpRequest::create({HttpMethod::Get, Url::create("").take(), {}, {},
                                HeaderList(std::move(hdrs)), Body::none(), Auth::none()})
        .take();
  }
};

template <> struct RequestTraits<GrpcRequest> {
  static constexpr RequestType type = RequestType::Grpc;
  static RequestModel::Payload makeDefault() {
    GrpcRequest::Parts gp;
    gp.message = JsonText::of("{}");
    // gRPC has no Auth tab: call-level auth IS metadata, so seed off-by-default hints
    gp.metadata = GrpcMetadata::create({{"authorization", "Bearer <token>", false},
                                        {"x-api-key", "<api-key>", false},
                                        {"x-request-id", "<uuid>", false}})
                      .take();
    return GrpcRequest::create(std::move(gp)).take();
  }
};

template <> struct RequestTraits<GraphQlRequest> {
  static constexpr RequestType type = RequestType::GraphQl;
  static RequestModel::Payload makeDefault() {
    GraphQlOperation op;
    op.query = "query {\n  \n}";
    return GraphQlRequest::create(
               {Url::create("").take(), op, {}, Auth::none(), GqlSubTransport::Http, ""})
        .take();
  }
};

template <> struct RequestTraits<WebSocketRequest> {
  static constexpr RequestType type = RequestType::WebSocket;
  static RequestModel::Payload makeDefault() {
    return WebSocketRequest::create(
               {Url::create("").take(), {}, {}, Auth::none(), {}, WsSendKind::Text})
        .take();
  }
};

template <> struct RequestTraits<KafkaRequest> {
  static constexpr RequestType type = RequestType::Kafka;
  static RequestModel::Payload makeDefault() {
    // BrokerList/KafkaTopic reject empty -> seed placeholders
    KafkaProduceConfig cfg{KafkaTopic::create("demo-topic").take()};
    KafkaMessage msg;
    msg.value = MessagePayload{"{}"};
    auto mode = KafkaRequest::Mode{KafkaProduceSpec{std::move(cfg), std::move(msg)}};
    return KafkaRequest::create(BrokerList::parse("localhost:9092").take(),
                                KafkaSecurity::plaintext(), std::move(mode))
        .take();
  }
};

template <> struct RequestTraits<SoapRequest> {
  static constexpr RequestType type = RequestType::Soap;
  static RequestModel::Payload makeDefault() {
    SoapRequest::Parts sp{Url::create("").take()};
    sp.envelope = "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
                  "  <soapenv:Header/>\n"
                  "  <soapenv:Body>\n"
                  "  </soapenv:Body>\n"
                  "</soapenv:Envelope>";
    return SoapRequest::create(std::move(sp)).take();
  }
};

template <> struct RequestTraits<LdapRequest> {
  static constexpr RequestType type = RequestType::Ldap;
  static RequestModel::Payload makeDefault() {
    LdapRequest::Parts lp{Url::create("ldap://localhost:389").take()};
    lp.baseDn = "dc=example,dc=com";
    lp.filter = "(uid=bob)";
    return LdapRequest::create(std::move(lp)).take();
  }
};

namespace traits_detail {

inline constexpr std::size_t kPayloadCount = std::variant_size_v<RequestModel::Payload>;
template <std::size_t I> using Alt = std::variant_alternative_t<I, RequestModel::Payload>;

template <std::size_t... I> constexpr bool aligned(std::index_sequence<I...>) {
  return ((static_cast<std::size_t>(RequestTraits<Alt<I>>::type) == I) && ...);
}
static_assert(aligned(std::make_index_sequence<kPayloadCount>{}),
              "RequestTraits<T>::type must equal T's index in RequestModel::Payload");
static_assert(kPayloadCount == ::core::kRequestTypeCount,
              "kRequestTypeTokens must cover every Payload alternative");

template <class F, std::size_t... I>
decltype(auto) dispatch(RequestType t, F &&f, std::index_sequence<I...>) {
  using R = decltype(std::forward<F>(f)(TypeTag<Alt<0>>{}));
  using Fn = R (*)(F &&);
  static const Fn table[] = {[](F &&g) -> R { return std::forward<F>(g)(TypeTag<Alt<I>>{}); }...};
  auto i = static_cast<std::size_t>(t);
  return table[i < kPayloadCount ? i : 0](std::forward<F>(f));
}

} // namespace traits_detail

// Runtime enum -> compile-time payload type (f receives a TypeTag); out-of-range clamps to Http (index 0).
template <class F> decltype(auto) dispatchType(RequestType t, F &&f) {
  return traits_detail::dispatch(t, std::forward<F>(f),
                                 std::make_index_sequence<traits_detail::kPayloadCount>{});
}

} // namespace core::domain
