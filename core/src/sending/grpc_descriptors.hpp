// grpc_descriptors.hpp — Build a shared DescriptorPool for GrpcSender and Engine::listGrpcMethods.
// Supports 3 sources: protoFiles (.proto + import paths), descriptorSet (FileDescriptorSet),
// reflection (gRPC ServerReflection). Keeps the source alive across the pass via DescriptorContext.
//
// INTERNAL header (core/src) — may leak protobuf/grpc types; do NOT include from core/include.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor_database.h>

#include <grpcpp/grpcpp.h>

#include "core/domain/grpc/grpc_method.hpp"  // domain GrpcMethodDescriptor / GrpcMethodType
#include "core/domain/grpc/grpc_request.hpp" // domain GrpcRequest payload
#include "core/domain/values/tls_config.hpp" // domain TlsConfig

namespace core::grpcdesc {

namespace gp = google::protobuf;

// Collect .proto compile errors into a string to report back to the UI.
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
};

// Create channel credentials (insecure / TLS).
std::shared_ptr<grpc::ChannelCredentials> makeCreds(const core::domain::TlsConfig& tls);

// Build descriptors per g.protoSource(). Returns false + ctx.error on failure.
bool buildDescriptors(const core::domain::GrpcRequest& g, DescriptorContext& ctx);

// List services/methods from a built context (used for the RPC-selection dropdown).
std::vector<core::domain::GrpcMethodDescriptor> listMethods(const DescriptorContext& ctx);

// unary | server_streaming | client_streaming | bidi_streaming.
std::string methodTypeOf(const gp::MethodDescriptor* m);

} // namespace core::grpcdesc
