// grpc_sender.hpp — GrpcSender: DYNAMIC unary (README §7.3, §12.3).
// DescriptorPool + DynamicMessageFactory + GenericStub, marshal JSON<->protobuf at runtime.
// POC: unary only; protoSource = reflection | protoFiles | descriptorSet (see grpc_descriptors).
#pragma once

#include "core/sending/i_request_sender.hpp"

namespace core {

class GrpcSender : public IRequestSender {
public:
    void send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
              const std::shared_ptr<CancelToken>& cancel) override;
};

} // namespace core
