#pragma once

#include <string>
#include <utility>
#include <variant>

#include "core/domain/common/result.hpp"
#include "core/domain/graphql/graphql_request.hpp"
#include "core/domain/grpc/grpc_request.hpp"
#include "core/domain/http/http_request.hpp"
#include "core/domain/kafka/kafka_request.hpp"
#include "core/domain/ldap/ldap_request.hpp"
#include "core/domain/request/request_config.hpp"
#include "core/domain/request/request_id.hpp"
#include "core/domain/request/request_type.hpp"
#include "core/domain/soap/soap_request.hpp"
#include "core/domain/ws/websocket_request.hpp"

namespace core::domain {

using RequestType = ::core::RequestType;

class RequestModel {
public:
  // Order MUST match RequestType so type() == variant index (new types append at the end).
  using Payload = std::variant<HttpRequest, GrpcRequest, GraphQlRequest, WebSocketRequest,
                               KafkaRequest, SoapRequest, LdapRequest>;

  // id may be empty: a draft has no identity until persisted (the store assigns one on save).
  static Result<RequestModel> create(RequestId id, std::string name, int seq, RequestConfig cfg,
                                     Payload payload) {
    return Result<RequestModel>::ok(
        RequestModel(std::move(id), std::move(name), seq, cfg, std::move(payload)));
  }

  const RequestId &id() const noexcept { return id_; }
  const std::string &name() const noexcept { return name_; }
  int seq() const noexcept { return seq_; }
  const RequestConfig &config() const noexcept { return config_; }
  const Payload &payload() const noexcept { return payload_; }

  RequestType type() const noexcept { return static_cast<RequestType>(payload_.index()); }

  template <class V> decltype(auto) match(V &&v) const { return std::visit(std::forward<V>(v), payload_); }

  RequestModel withName(std::string name) const {
    RequestModel c = *this;
    c.name_ = std::move(name);
    return c;
  }
  RequestModel withPayload(Payload payload) const {
    RequestModel c = *this;
    c.payload_ = std::move(payload);
    return c;
  }

  bool operator==(const RequestModel &o) const {
    return id_ == o.id_ && name_ == o.name_ && seq_ == o.seq_ && config_ == o.config_ &&
           payload_ == o.payload_;
  }
  bool operator!=(const RequestModel &o) const { return !(*this == o); }

private:
  RequestModel(RequestId id, std::string name, int seq, RequestConfig cfg, Payload payload)
      : id_(std::move(id)), name_(std::move(name)), seq_(seq), config_(cfg),
        payload_(std::move(payload)) {}

  RequestId id_;
  std::string name_;
  int seq_;
  RequestConfig config_;
  Payload payload_;
};

} // namespace core::domain
