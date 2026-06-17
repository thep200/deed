// grpc_descriptors.hpp — Dựng DescriptorPool dùng chung cho GrpcSender và Engine::listGrpcMethods.
// Hỗ trợ 3 nguồn: protoFiles (.proto + import paths), descriptorSet (FileDescriptorSet),
// reflection (gRPC ServerReflection). Giữ nguồn sống suốt vòng dùng qua DescriptorContext.
//
// Header NỘI BỘ (core/src) — được phép leak kiểu protobuf/grpc; KHÔNG include từ core/include.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor_database.h>

#include <grpcpp/grpcpp.h>

#include "core/types.hpp"

namespace core::grpcdesc {

namespace gp = google::protobuf;

// Gom lỗi compile .proto thành chuỗi để báo về UI.
class ProtoErrorCollector : public gp::compiler::MultiFileErrorCollector {
public:
    std::string errors;
    void RecordError(absl::string_view filename, int line, int column,
                     absl::string_view message) override;
};

// Giữ mọi nguồn descriptor sống suốt vòng gửi/liệt kê.
// Thứ tự khai báo quan trọng: pool (tham chiếu db) phải khai báo SAU db để huỷ TRƯỚC.
struct DescriptorContext {
    // protoFiles
    std::unique_ptr<gp::compiler::DiskSourceTree> sourceTree;
    std::unique_ptr<ProtoErrorCollector> errCollector;
    std::unique_ptr<gp::compiler::Importer> importer;
    // descriptorSet
    std::unique_ptr<gp::DescriptorPool> pool;
    // reflection
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<gp::DescriptorDatabase> reflectionDb; // khai báo TRƯỚC reflectionPool
    std::unique_ptr<gp::DescriptorPool> reflectionPool;

    const gp::DescriptorPool* activePool = nullptr;
    std::vector<std::string> serviceNames; // full name của mọi service phát hiện được
    std::string error;
};

// Tạo credentials cho channel (insecure / TLS).
std::shared_ptr<grpc::ChannelCredentials> makeCreds(const GrpcTls& tls);

// Dựng descriptor theo g.protoSource.mode. Trả false + ctx.error nếu lỗi.
bool buildDescriptors(const GrpcRequest& g, DescriptorContext& ctx);

// Liệt kê service/method từ context đã build (dùng cho dropdown chọn RPC).
std::vector<GrpcMethodInfo> listMethods(const DescriptorContext& ctx);

// unary | server_streaming | client_streaming | bidi_streaming.
std::string methodTypeOf(const gp::MethodDescriptor* m);

} // namespace core::grpcdesc
