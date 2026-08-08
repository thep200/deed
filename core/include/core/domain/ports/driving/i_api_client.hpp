#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/graphql/gql_schema.hpp"
#include "core/domain/grpc/grpc_method.hpp"
#include "core/domain/grpc/grpc_request.hpp"
#include "core/domain/ports/driving/exec_id.hpp"
#include "core/domain/ports/driven/i_request_observer.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/values/json_text.hpp"
#include "core/domain/ws/ws_message.hpp"

namespace core::domain {

class IApiClient {
public:
  virtual ~IApiClient() = default;

  // The observer receives every event for this execution's lifecycle.
  virtual Result<RequestExecutionId> send(const RequestModel &request,
                                          std::shared_ptr<IRequestObserver> observer) = 0;

  virtual Status cancel(RequestExecutionId exec) = 0;
  // Kill every in-flight execution — must work even when the caller's exec handle is stale or not stored yet.
  virtual Status cancelAll() = 0;

  // Interactive streaming / WS (Unsupported if the type doesn't allow it).
  virtual Status sendStreamMessage(RequestExecutionId exec, WsMessage msg) = 0;
  virtual Status halfClose(RequestExecutionId exec) = 0;
  virtual Status closeStream(RequestExecutionId exec, int code, std::string reason) = 0;

  // Non-sending use-cases (synchronous, pure domain).
  virtual Status validateJson(const JsonText &) = 0;
  virtual Result<std::vector<GrpcMethodDescriptor>> listGrpcMethods(const GrpcRequest &) = 0;
  // Synchronous — call off-main.
  virtual Result<GqlSchema> introspectGraphQl(const RequestModel &) = 0;
};

} // namespace core::domain
