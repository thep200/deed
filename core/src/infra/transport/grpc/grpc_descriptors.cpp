#include "infra/transport/grpc/grpc_descriptors.hpp"

#include <chrono>
#include <mutex>
#include <unordered_set>

#include <google/protobuf/descriptor.pb.h>

#include <grpcpp/support/sync_stream.h>

#include "infra/platform/fs_util.hpp"
#include "infra/transport/grpc/grpc_method_listing.hpp"

// Reflection stub generated from third_party/grpc_reflection/reflection.proto (codegen in CMake).
#include "reflection.grpc.pb.h"
#include "reflection.pb.h"

namespace core::grpcdesc {

namespace {

namespace refl = grpc::reflection::v1alpha;
namespace d = core::domain;

// Overload set for ProtoSource::match (visits the variant alternatives).
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// Lazy DescriptorDatabase that fetches FileDescriptorProto via gRPC ServerReflection.
// Keeps one bidi-stream alive, caches received files so DescriptorPool can resolve transitive imports.
// Single-threaded use within one send/list pass (a fresh instance per pass).
class ReflectionDescriptorDatabase : public gp::DescriptorDatabase {
public:
    ReflectionDescriptorDatabase(std::shared_ptr<grpc::Channel> channel,
                                 std::shared_ptr<core::CancelToken> cancel)
        : channel_(std::move(channel)), stub_(refl::ServerReflection::NewStub(channel_)),
          cancel_(std::move(cancel)) {}

    ~ReflectionDescriptorDatabase() override {
        if (stream_) {
            // TryCancel first (H7): a half-open/hung reflection server would otherwise make Finish() block
            // forever, hanging DescriptorContext teardown and the whole send. The deadline on ctx_ (set in
            // stream()) is the second backstop.
            if (ctx_) ctx_->TryCancel();
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
        return false; // POC: extensions not supported.
    }

    // List the full name of every service registered on the server.
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
            ctx_ = std::make_shared<grpc::ClientContext>();
            // Cap total reflection time (H7) so a silent/hung server can't block the call indefinitely.
            ctx_->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
            // Cancel must not wait out that deadline: kill the reflection RPC the moment the user asks.
            // weak_ptr -> a late cancel after this db died is a no-op, not a dangling TryCancel.
            if (cancel_)
                cancel_->onCancel([w = std::weak_ptr<grpc::ClientContext>(ctx_)] {
                    if (auto c = w.lock()) c->TryCancel();
                });
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
    std::shared_ptr<core::CancelToken> cancel_;
    std::shared_ptr<grpc::ClientContext> ctx_;
    std::shared_ptr<Stream> stream_;
    gp::SimpleDescriptorDatabase cached_;
    std::unordered_set<std::string> known_files_;
    std::unordered_set<std::string> missing_symbols_;
    std::string lastError_;
    std::mutex mu_;
};

// Collect service full_names from a FileDescriptor (skip if it has none).
void collectServices(const gp::FileDescriptor* file, std::vector<std::string>& out) {
    if (!file) return;
    for (int i = 0; i < file->service_count(); ++i)
        out.push_back(std::string(file->service(i)->full_name()));
}

bool buildFromProtoFiles(const d::ProtoFiles& ps, DescriptorContext& ctx) {
    ctx.sourceTree = std::make_unique<gp::compiler::DiskSourceTree>();
    if (ps.importPaths.empty()) {
        ctx.sourceTree->MapPath("", "."); // default to cwd
    }
    for (const auto& ip : ps.importPaths) ctx.sourceTree->MapPath("", ip);
    ctx.errCollector = std::make_unique<ProtoErrorCollector>();
    ctx.importer = std::make_unique<gp::compiler::Importer>(ctx.sourceTree.get(),
                                                            ctx.errCollector.get());
    for (const auto& f : ps.protoFiles) {
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

bool buildFromDescriptorSet(const d::ProtoDescriptorSet& ps, DescriptorContext& ctx) {
    std::string raw;
    if (!fsutil::readFile(ps.descriptorSetPath, raw)) {
        ctx.error = "cannot read descriptorSet: " + ps.descriptorSetPath;
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

bool buildFromReflection(const std::string& target, const d::TlsConfig& tls, DescriptorContext& ctx) {
    if (target.empty()) {
        ctx.error = "reflection: target (host:port) is empty";
        return false;
    }
    ctx.channel = grpc::CreateChannel(target, makeCreds(tls));
    auto db = std::make_unique<ReflectionDescriptorDatabase>(ctx.channel, ctx.cancel);
    if (!db->getServices(&ctx.serviceNames)) {
        ctx.error = db->lastError().empty() ? "reflection: ListServices failed" : db->lastError();
        return false;
    }
    ctx.reflectionDb = std::move(db); // downcast to base; GetServices is done so the concrete type is no longer needed
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

std::shared_ptr<grpc::ChannelCredentials> makeCreds(const d::TlsConfig& tls) {
    if (!tls.enabled()) return grpc::InsecureChannelCredentials();
    grpc::SslCredentialsOptions opts;
    std::string buf;
    if (!tls.caCertPath().empty() && fsutil::readFile(tls.caCertPath(), buf)) opts.pem_root_certs = buf;
    if (!tls.clientKeyPath().empty() && fsutil::readFile(tls.clientKeyPath(), buf)) opts.pem_private_key = buf;
    if (!tls.clientCertPath().empty() && fsutil::readFile(tls.clientCertPath(), buf)) opts.pem_cert_chain = buf;
    return grpc::SslCredentials(opts);
}

bool buildDescriptors(const d::GrpcRequest& g, DescriptorContext& ctx) {
    return g.protoSource().match(overloaded{
        [&](const d::ProtoReflection&) { return buildFromReflection(g.target(), g.tls(), ctx); },
        [&](const d::ProtoFiles& ps) { return buildFromProtoFiles(ps, ctx); },
        [&](const d::ProtoDescriptorSet& ps) { return buildFromDescriptorSet(ps, ctx); },
    });
}

namespace {
d::GrpcMethodType methodTypeEnumOf(const gp::MethodDescriptor* m) {
    bool c = m->client_streaming(), s = m->server_streaming();
    if (c && s) return d::GrpcMethodType::BidiStreaming;
    if (c) return d::GrpcMethodType::ClientStreaming;
    if (s) return d::GrpcMethodType::ServerStreaming;
    return d::GrpcMethodType::Unary;
}
} // namespace

std::vector<d::GrpcMethodDescriptor> listMethods(const DescriptorContext& ctx) {
    std::vector<d::GrpcMethodDescriptor> out;
    if (!ctx.activePool) return out;
    for (const auto& sname : ctx.serviceNames) {
        // Hide the server's own reflection service (any version: v1, v1alpha, ...).
        if (sname.rfind("grpc.reflection.", 0) == 0) continue;
        const gp::ServiceDescriptor* svc = ctx.activePool->FindServiceByName(sname);
        if (!svc) continue;
        for (int i = 0; i < svc->method_count(); ++i) {
            const gp::MethodDescriptor* m = svc->method(i);
            out.push_back(d::GrpcMethodDescriptor{std::string(svc->full_name()),
                                                  std::string(m->name()), methodTypeEnumOf(m)});
        }
    }
    return out;
}

std::vector<d::GrpcMethodDescriptor> listGrpcMethods(const d::GrpcRequest& g, std::string& err) {
    DescriptorContext ctx;
    if (!buildDescriptors(g, ctx)) { err = ctx.error; return {}; }
    return listMethods(ctx);
}

} // namespace core::grpcdesc
