// May leak protobuf/grpc types — do NOT include from core/include.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor_database.h>

#include <grpcpp/grpcpp.h>

#include "core/domain/grpc/grpc_method.hpp"
#include "core/domain/grpc/grpc_request.hpp"
#include "core/domain/values/tls_config.hpp"
#include "infra/transport/shared/cancel_token.hpp"

namespace core::grpcdesc {

namespace gp = google::protobuf;

class ProtoErrorCollector : public gp::compiler::MultiFileErrorCollector {
public:
    std::string errors;
    void RecordError(absl::string_view filename, int line, int column,
                     absl::string_view message) override;
};

// Keep every descriptor source alive across the send/list pass.
// Declaration order matters: pool (references db) must be declared AFTER db so it destructs FIRST.
struct DescriptorContext {
    // protoFiles
    std::unique_ptr<gp::compiler::DiskSourceTree> sourceTree;
    std::unique_ptr<ProtoErrorCollector> errCollector;
    std::unique_ptr<gp::compiler::Importer> importer;
    // descriptorSet
    std::unique_ptr<gp::DescriptorPool> pool;
    // reflection
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<gp::DescriptorDatabase> reflectionDb; // declared BEFORE reflectionPool
    std::unique_ptr<gp::DescriptorPool> reflectionPool;

    const gp::DescriptorPool* activePool = nullptr;
    std::vector<std::string> serviceNames; // full name of every discovered service
    std::string error;
    // Optional. Reflection blocks on the network; without this Cancel waits out the 30s stream deadline.
    std::shared_ptr<core::CancelToken> cancel;
};

std::shared_ptr<grpc::ChannelCredentials> makeCreds(const core::domain::TlsConfig& tls);

// Returns false + ctx.error on failure.
bool buildDescriptors(const core::domain::GrpcRequest& g, DescriptorContext& ctx);

std::vector<core::domain::GrpcMethodDescriptor> listMethods(const DescriptorContext& ctx);

} // namespace core::grpcdesc
