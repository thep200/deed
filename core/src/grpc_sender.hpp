// grpc_sender.hpp — GrpcSender: unary ĐỘNG (README §7.3, §12.3).
// DescriptorPool + DynamicMessageFactory + GenericStub, marshal JSON<->protobuf runtime.
// POC: chỉ unary; protoSource = protoFiles | descriptorSet (reflection: chưa hỗ trợ ở build này).
#pragma once

#include "core/i_request_sender.hpp"

namespace core {

class GrpcSender : public IRequestSender {
public:
    void send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
              const std::shared_ptr<CancelToken>& cancel) override;
};

} // namespace core
