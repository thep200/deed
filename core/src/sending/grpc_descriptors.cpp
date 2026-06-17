#include "sending/grpc_descriptors.hpp"

#include <mutex>
#include <unordered_set>

#include <google/protobuf/descriptor.pb.h>

#include <grpcpp/support/sync_stream.h>

#include "infra/fs_util.hpp"

// Stub reflection sinh từ third_party/grpc_reflection/reflection.proto (codegen trong CMake).
#include "reflection.grpc.pb.h"
#include "reflection.pb.h"

namespace core::grpcdesc {

namespace {

namespace refl = grpc::reflection::v1alpha;

// DescriptorDatabase lười, lấy FileDescriptorProto qua gRPC ServerReflection.
// Giữ 1 bidi-stream sống, cache file đã nhận để DescriptorPool resolve transitive imports.
// Dùng đơn luồng trong 1 vòng send/list (mỗi vòng 1 instance riêng).
class ReflectionDescriptorDatabase : public gp::DescriptorDatabase {
public:
    explicit ReflectionDescriptorDatabase(std::shared_ptr<grpc::Channel> channel)
        : channel_(std::move(channel)), stub_(refl::ServerReflection::NewStub(channel_)) {}

    ~ReflectionDescriptorDatabase() override {
        if (stream_) {
            stream_->WritesDone();
            grpc::Status s = stream_->Finish();
            (void)s;
        }
    }

    bool FindFileByName(const std::string& filename, gp::FileDescriptorProto* output) override {
        if (known_files_.count(filename)) return cached_.FindFileByName(filename, output);
        refl::ServerReflectionRequest req;
        req.set_file_by_filename(filename);
        refl::ServerReflectionResponse resp;
        if (!doOneRequest(req, resp)) return false;
        addFiles(resp);
        return cached_.FindFileByName(filename, output);
    }

    bool FindFileContainingSymbol(const std::string& symbol, gp::FileDescriptorProto* output) override {
        if (missing_symbols_.count(symbol)) return false;
        if (cached_.FindFileContainingSymbol(symbol, output)) return true;
        refl::ServerReflectionRequest req;
        req.set_file_containing_symbol(symbol);
        refl::ServerReflectionResponse resp;
        if (!doOneRequest(req, resp)) { missing_symbols_.insert(symbol); return false; }
        if (resp.message_response_case() == refl::ServerReflectionResponse::kErrorResponse) {
            missing_symbols_.insert(symbol);
            return false;
        }
        addFiles(resp);
        return cached_.FindFileContainingSymbol(symbol, output);
    }

    bool FindFileContainingExtension(const std::string&, int, gp::FileDescriptorProto*) override {
        return false; // POC: không hỗ trợ extension.
    }

    // Liệt kê full name của mọi service đăng ký trên server.
    bool getServices(std::vector<std::string>* out) {
        refl::ServerReflectionRequest req;
        req.set_list_services("*");
        refl::ServerReflectionResponse resp;
        if (!doOneRequest(req, resp)) return false;
        if (resp.message_response_case() != refl::ServerReflectionResponse::kListServicesResponse)
            return false;
        for (const auto& svc : resp.list_services_response().service()) out->push_back(svc.name());
        return true;
    }

    const std::string& lastError() const { return lastError_; }

private:
    using Stream = grpc::ClientReaderWriter<refl::ServerReflectionRequest,
                                            refl::ServerReflectionResponse>;

    Stream* stream() {
        if (!stream_) {
            ctx_ = std::make_unique<grpc::ClientContext>();
            stream_ = stub_->ServerReflectionInfo(ctx_.get());
        }
        return stream_.get();
    }

    bool doOneRequest(const refl::ServerReflectionRequest& req, refl::ServerReflectionResponse& resp) {
        std::lock_guard<std::mutex> lk(mu_);
        Stream* s = stream();
        if (!s->Write(req)) { lastError_ = "reflection: stream write failed"; return false; }
        if (!s->Read(&resp)) { lastError_ = "reflection: stream read failed (server may not support reflection)"; return false; }
        return true;
    }

    void addFiles(const refl::ServerReflectionResponse& resp) {
        if (resp.message_response_case() != refl::ServerReflectionResponse::kFileDescriptorResponse)
            return;
        for (const auto& raw : resp.file_descriptor_response().file_descriptor_proto()) {
            gp::FileDescriptorProto fdp;
            if (!fdp.ParseFromString(raw)) continue;
            if (known_files_.insert(fdp.name()).second) cached_.Add(fdp);
        }
    }

    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<refl::ServerReflection::Stub> stub_;
    std::unique_ptr<grpc::ClientContext> ctx_;
    std::shared_ptr<Stream> stream_;
    gp::SimpleDescriptorDatabase cached_;
    std::unordered_set<std::string> known_files_;
    std::unordered_set<std::string> missing_symbols_;
    std::string lastError_;
    std::mutex mu_;
};

// Gom service full_name từ một FileDescriptor (kể cả không có service thì bỏ qua).
void collectServices(const gp::FileDescriptor* file, std::vector<std::string>& out) {
    if (!file) return;
    for (int i = 0; i < file->service_count(); ++i)
        out.push_back(std::string(file->service(i)->full_name()));
}

bool buildFromProtoFiles(const GrpcRequest& g, DescriptorContext& ctx) {
    ctx.sourceTree = std::make_unique<gp::compiler::DiskSourceTree>();
    if (g.protoSource.importPaths.empty()) {
        ctx.sourceTree->MapPath("", "."); // mặc định cwd
    }
    for (const auto& ip : g.protoSource.importPaths) ctx.sourceTree->MapPath("", ip);
    ctx.errCollector = std::make_unique<ProtoErrorCollector>();
    ctx.importer = std::make_unique<gp::compiler::Importer>(ctx.sourceTree.get(),
                                                            ctx.errCollector.get());
    for (const auto& f : g.protoSource.files) {
        const gp::FileDescriptor* fd = ctx.importer->Import(f);
        if (fd == nullptr) {
            ctx.error = "failed to load .proto: " + ctx.errCollector->errors;
            return false;
        }
        collectServices(fd, ctx.serviceNames);
    }
    ctx.activePool = ctx.importer->pool();
    return true;
}

bool buildFromDescriptorSet(const GrpcRequest& g, DescriptorContext& ctx) {
    std::string raw;
    if (!fsutil::readFile(g.protoSource.descriptorSetPath, raw)) {
        ctx.error = "cannot read descriptorSet: " + g.protoSource.descriptorSetPath;
        return false;
    }
    gp::FileDescriptorSet fds;
    if (!fds.ParseFromString(raw)) {
        ctx.error = "invalid descriptorSet (parse failed)";
        return false;
    }
    ctx.pool = std::make_unique<gp::DescriptorPool>();
    for (const auto& file : fds.file()) {
        const gp::FileDescriptor* fd = ctx.pool->BuildFile(file);
        if (fd == nullptr) {
            ctx.error = "failed to build FileDescriptor: " + file.name();
            return false;
        }
        collectServices(fd, ctx.serviceNames);
    }
    ctx.activePool = ctx.pool.get();
    return true;
}

bool buildFromReflection(const GrpcRequest& g, DescriptorContext& ctx) {
    if (g.target.empty()) {
        ctx.error = "reflection: target (host:port) is empty";
        return false;
    }
    ctx.channel = grpc::CreateChannel(g.target, makeCreds(g.tls));
    auto db = std::make_unique<ReflectionDescriptorDatabase>(ctx.channel);
    if (!db->getServices(&ctx.serviceNames)) {
        ctx.error = db->lastError().empty() ? "reflection: ListServices failed" : db->lastError();
        return false;
    }
    ctx.reflectionDb = std::move(db); // hạ về base; GetServices đã gọi xong nên không cần kiểu cụ thể nữa
    ctx.reflectionPool = std::make_unique<gp::DescriptorPool>(ctx.reflectionDb.get());
    ctx.activePool = ctx.reflectionPool.get();
    return true;
}

} // namespace

void ProtoErrorCollector::RecordError(absl::string_view filename, int line, int column,
                                      absl::string_view message) {
    errors += std::string(filename) + ":" + std::to_string(line) + ":" +
              std::to_string(column) + ": " + std::string(message) + "\n";
}

std::shared_ptr<grpc::ChannelCredentials> makeCreds(const GrpcTls& tls) {
    if (!tls.enabled) return grpc::InsecureChannelCredentials();
    grpc::SslCredentialsOptions opts;
    std::string buf;
    if (!tls.caCertPath.empty() && fsutil::readFile(tls.caCertPath, buf)) opts.pem_root_certs = buf;
    if (!tls.clientKeyPath.empty() && fsutil::readFile(tls.clientKeyPath, buf)) opts.pem_private_key = buf;
    if (!tls.clientCertPath.empty() && fsutil::readFile(tls.clientCertPath, buf)) opts.pem_cert_chain = buf;
    return grpc::SslCredentials(opts);
}

bool buildDescriptors(const GrpcRequest& g, DescriptorContext& ctx) {
    if (g.protoSource.mode == "protoFiles") return buildFromProtoFiles(g, ctx);
    if (g.protoSource.mode == "descriptorSet") return buildFromDescriptorSet(g, ctx);
    if (g.protoSource.mode == "reflection") return buildFromReflection(g, ctx);
    ctx.error = "unknown protoSource mode: " + g.protoSource.mode;
    return false;
}

std::string methodTypeOf(const gp::MethodDescriptor* m) {
    bool c = m->client_streaming(), s = m->server_streaming();
    if (c && s) return "bidi_streaming";
    if (c) return "client_streaming";
    if (s) return "server_streaming";
    return "unary";
}

std::vector<GrpcMethodInfo> listMethods(const DescriptorContext& ctx) {
    std::vector<GrpcMethodInfo> out;
    if (!ctx.activePool) return out;
    for (const auto& sname : ctx.serviceNames) {
        // Ẩn chính service reflection của server (mọi version: v1, v1alpha, ...).
        if (sname.rfind("grpc.reflection.", 0) == 0) continue;
        const gp::ServiceDescriptor* svc = ctx.activePool->FindServiceByName(sname);
        if (!svc) continue;
        for (int i = 0; i < svc->method_count(); ++i) {
            const gp::MethodDescriptor* m = svc->method(i);
            out.push_back(GrpcMethodInfo{std::string(svc->full_name()),
                                         std::string(m->name()), methodTypeOf(m)});
        }
    }
    return out;
}

} // namespace core::grpcdesc
