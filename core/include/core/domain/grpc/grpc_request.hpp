// core/domain/grpc/grpc_request.hpp — GrpcRequest aggregate payload (REFACTOR_SPEC §5.5).
#pragma once

#include <string>
#include <utility>

#include "core/domain/common/result.hpp"
#include "core/domain/grpc/grpc_metadata.hpp"
#include "core/domain/grpc/grpc_method.hpp"
#include "core/domain/grpc/proto_source.hpp"
#include "core/domain/values/json_text.hpp"
#include "core/domain/values/tls_config.hpp"

namespace core::domain {

class GrpcRequest {
public:
  struct Parts {
    std::string target;  // "host:port" authority (no scheme, no path)
    std::string service; // "pkg.Service"
    std::string method;  // RPC name
    GrpcMethodType methodType = GrpcMethodType::Unary;
    JsonText message = JsonText::emptyObject(); // request message as JSON text
    GrpcMetadata metadata = GrpcMetadata::empty();
    ProtoSource protoSource = ProtoSource::reflection();
    TlsConfig tls = TlsConfig::disabled();
  };

  // target/service/method may all be empty on a DRAFT (no host typed / no RPC picked yet) — emptiness is a
  // send-time concern, not a construction invariant (consistent with Url/RequestId allowing empty drafts).
  // The orchestrator/sender blocks an actual send with an empty target/method.
  static Result<GrpcRequest> create(Parts p) {
    return Result<GrpcRequest>::ok(GrpcRequest(std::move(p)));
  }

  const std::string &target() const noexcept { return p_.target; }
  const std::string &service() const noexcept { return p_.service; }
  const std::string &method() const noexcept { return p_.method; }
  GrpcMethodType methodType() const noexcept { return p_.methodType; }
  const JsonText &message() const noexcept { return p_.message; }
  const GrpcMetadata &metadata() const noexcept { return p_.metadata; }
  const ProtoSource &protoSource() const noexcept { return p_.protoSource; }
  const TlsConfig &tls() const noexcept { return p_.tls; }

  bool operator==(const GrpcRequest &o) const {
    return p_.target == o.p_.target && p_.service == o.p_.service && p_.method == o.p_.method &&
           p_.methodType == o.p_.methodType && p_.message == o.p_.message &&
           p_.metadata == o.p_.metadata && p_.protoSource == o.p_.protoSource && p_.tls == o.p_.tls;
  }
  bool operator!=(const GrpcRequest &o) const { return !(*this == o); }

private:
  explicit GrpcRequest(Parts p) : p_(std::move(p)) {}
  Parts p_;
};

} // namespace core::domain
