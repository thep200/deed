// core/domain/request/request_defaults.hpp — domain policy for what a FRESH request of each protocol looks
// like (default headers, GraphQL starter document, Kafka placeholder broker/topic). Every creation path
// (persistence store, import, UI, tests) builds new requests from this single factory.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/domain/request/request_model.hpp"

namespace core::domain {

// Default payload for a freshly created request of `type`.
inline RequestModel::Payload defaultPayloadFor(RequestType type) {
  switch (type) {
  case RequestType::WebSocket:
    return WebSocketRequest::create(
               {Url::create("").take(), {}, {}, Auth::none(), {}, WsSendKind::Text})
        .take();
  case RequestType::GraphQl: {
    GraphQlOperation op;
    op.query = "query {\n  \n}"; // starter document
    return GraphQlRequest::create(
               {Url::create("").take(), op, {}, Auth::none(), GqlSubTransport::Http, ""})
        .take();
  }
  case RequestType::Grpc: {
    GrpcRequest::Parts gp; // reflection + unary + empty target are the Parts defaults
    gp.message = JsonText::of("{}");
    // Example metadata as OFF-by-default hints (same convention as the HTTP default headers below).
    // gRPC has no Auth tab: call-level auth IS metadata (`authorization`), channel TLS lives in TlsConfig.
    gp.metadata = GrpcMetadata::create({{"authorization", "Bearer <token>", false},
                                        {"x-api-key", "<api-key>", false},
                                        {"x-request-id", "<uuid>", false}})
                      .take();
    return GrpcRequest::create(std::move(gp)).take();
  }
  case RequestType::Kafka: {
    // BrokerList/KafkaTopic reject empty (SPEC_kafka §3 invariants) -> seed a placeholder, same convention
    // GraphQL uses for its non-empty-query invariant (not an "always-empty-ok" draft like url/target).
    KafkaProduceConfig cfg{KafkaTopic::create("demo-topic").take()};
    KafkaMessage msg;
    msg.value = MessagePayload{"{}"};
    auto mode = KafkaRequest::Mode{KafkaProduceSpec{std::move(cfg), std::move(msg)}};
    return KafkaRequest::create(BrokerList::parse("localhost:9092").take(),
                                KafkaSecurity::plaintext(), std::move(mode))
        .take();
  }
  default: {
    // HTTP: GET + the common default headers as OFF-by-default hints (User-Agent on), no body.
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
  }
}

} // namespace core::domain
