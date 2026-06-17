#include "core/engine.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#include "core/sending/i_request_sender.hpp"
#include "core/variables/variable_resolver.hpp"
#include "infra/fs_util.hpp"
#include "sending/grpc_sender.hpp"
#include "sending/http_sender.hpp"
#include "codec/json_codec.hpp"
#include "sending/sender_registry.hpp"
#include "infra/thread_pool.hpp"

namespace fs = std::filesystem;

namespace core {

namespace {

// Resolve {{var}} cho 1 mảng KeyValue (value).
void resolveKv(std::vector<KeyValue>& kvs, const std::map<std::string, std::string>& vars) {
    for (auto& kv : kvs) kv.value = VariableResolver::resolve(kv.value, vars).text;
}

std::string resolveStr(const std::string& s, const std::map<std::string, std::string>& vars) {
    return VariableResolver::resolve(s, vars).text;
}

} // namespace

struct Engine::Impl {
    explicit Impl(EngineConfig cfg)
        : collectionRoot(std::move(cfg.collectionRoot)),
          collection(collectionRoot),
          session(collectionRoot),
          secrets(std::make_shared<FileSecretStore>(collectionRoot)),
          environments(collectionRoot, secrets),
          appConfig(cfg.appConfigPath.empty() ? AppConfigStore()
                                              : AppConfigStore(cfg.appConfigPath)) {
        registry.registerSender(RequestType::Http, std::make_unique<HttpSender>());
        registry.registerSender(RequestType::Grpc, std::make_unique<GrpcSender>());
    }

    std::string collectionRoot;
    CollectionStore collection;
    SessionStore session;
    std::shared_ptr<FileSecretStore> secrets;
    EnvironmentStore environments;
    AppConfigStore appConfig;

    SenderRegistry registry;

    std::atomic<RequestHandle> nextHandle{1};

    // Inflight registry tách thành object có shared_ptr: worker giữ tham chiếu riêng,
    // KHÔNG deref this->impl_ (libc++ null-hoá unique_ptr trước khi chạy ~Impl, mà ~Impl
    // lại block ở pool.join() -> worker đọc impl_ null -> crash). Giữ qua shared_ptr là an toàn.
    struct InflightReg {
        std::mutex mu;
        std::map<RequestHandle, std::shared_ptr<CancelToken>> map;
    };
    std::shared_ptr<InflightReg> inflight = std::make_shared<InflightReg>();

    // pool KHAI BÁO CUỐI -> destructor chạy ĐẦU (reverse order): join hết worker
    // trước khi registry/inflight bị huỷ, đảm bảo sender còn sống suốt lúc gửi.
    ThreadPool pool;
};

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
    impl_->collection.ensureGitignore();
}
Engine::~Engine() = default;

void Engine::openCollection(const std::string& root) {
    impl_->collectionRoot = root;
    impl_->collection.setRoot(root);
    impl_->session.setRoot(root);
    impl_->secrets->setRoot(root);
    impl_->environments.setRoot(root);
    impl_->collection.ensureGitignore();
}

CollectionStore& Engine::collection() { return impl_->collection; }
SessionStore& Engine::session() { return impl_->session; }
EnvironmentStore& Engine::environments() { return impl_->environments; }
SecretStore& Engine::secrets() { return *impl_->secrets; }
AppConfigStore& Engine::appConfig() { return impl_->appConfig; }

std::map<std::string, std::string> Engine::activeVars() const {
    std::map<std::string, std::string> vars;
    auto merge = [&](const std::string& name) {
        try {
            Environment e = impl_->environments.load(name);
            for (const auto& k : e.keys)
                if (k.enabled) vars[k.key] = k.value;
        } catch (...) { /* env không tồn tại -> bỏ qua */ }
    };
    merge("Global");                         // nền
    std::string active = impl_->session.getActiveEnv();
    if (!active.empty() && active != "Global") merge(active); // override
    return vars;
}

ResolvedRequest Engine::resolveRequest(const RequestModel& model) const {
    auto vars = activeVars();
    ResolvedRequest rr;
    rr.model = model;

    // --- Merge settings precedence: request > folder > collection > app-global (README §12.1) ---
    AppConfig app = impl_->appConfig.load();
    if (model.type == RequestType::Http) {
        auto& s = rr.model.http.settings;
        if (!s.timeoutMsSet) { s.timeoutMs = app.defaultTimeoutMs; }
        if (!s.verifyTlsSet) { s.verifyTls = app.verifyTls; }
        // followRedirects: giữ mặc định true nếu chưa set.
    }

    // --- Resolve {{var}} mọi trường chuỗi ---
    if (rr.model.type == RequestType::Http) {
        auto& h = rr.model.http;
        h.url = resolveStr(h.url, vars);
        resolveKv(h.pathVariables, vars);
        resolveKv(h.params, vars);
        resolveKv(h.headers, vars);
        h.body.json = resolveStr(h.body.json, vars);
        h.body.text = resolveStr(h.body.text, vars);
        h.body.xml = resolveStr(h.body.xml, vars);
        h.body.graphqlQuery = resolveStr(h.body.graphqlQuery, vars);
        h.body.graphqlVariables = resolveStr(h.body.graphqlVariables, vars);
        resolveKv(h.body.formUrlEncoded, vars);
        h.auth.bearerToken = resolveStr(h.auth.bearerToken, vars);
        h.auth.basicUsername = resolveStr(h.auth.basicUsername, vars);
        h.auth.basicPassword = resolveStr(h.auth.basicPassword, vars);
        h.auth.apikeyValue = resolveStr(h.auth.apikeyValue, vars);
    } else {
        auto& g = rr.model.grpc;
        g.target = resolveStr(g.target, vars);
        g.message = resolveStr(g.message, vars);
        resolveKv(g.metadata, vars);
    }
    return rr;
}

RequestHandle Engine::send(const RequestModel& model, IUiDelegate* delegate) {
    RequestHandle handle = impl_->nextHandle.fetch_add(1);
    auto cancel = std::make_shared<CancelToken>();
    auto inflight = impl_->inflight; // shared_ptr — worker giữ tham chiếu riêng
    {
        std::lock_guard<std::mutex> lk(inflight->mu);
        inflight->map[handle] = cancel;
    }

    // Resolve + tra sender NGAY trên thread gọi (Engine còn sống) -> worker không deref impl_.
    IRequestSender* sender = impl_->registry.get(model.type);
    std::shared_ptr<ResolvedRequest> rr;
    std::shared_ptr<ApiError> immediateError;
    if (!sender) {
        immediateError = std::make_shared<ApiError>(
            ApiError{ErrorKind::Unsupported, "no sender for this request type"});
    } else {
        try {
            rr = std::make_shared<ResolvedRequest>(resolveRequest(model));
        } catch (const std::exception& e) {
            immediateError = std::make_shared<ApiError>(ApiError{ErrorKind::Unknown, e.what()});
        }
    }

    // Worker chỉ chạm biến capture (sender còn sống nhờ pool destroyed-first; inflight là shared_ptr).
    impl_->pool.submit([handle, delegate, cancel, inflight, sender, rr, immediateError]() {
        if (delegate) {
            if (immediateError) {
                delegate->onError(handle, *immediateError);
            } else {
                try {
                    sender->send(*rr, handle, *delegate, cancel);
                } catch (const std::exception& e) {
                    delegate->onError(handle, ApiError{ErrorKind::Unknown, e.what()});
                }
            }
        }
        std::lock_guard<std::mutex> lk(inflight->mu);
        inflight->map.erase(handle);
    });
    return handle;
}

void Engine::cancel(RequestHandle handle) {
    auto inflight = impl_->inflight;
    std::lock_guard<std::mutex> lk(inflight->mu);
    auto it = inflight->map.find(handle);
    if (it != inflight->map.end()) it->second->cancel();
}

// Import: importers thuần (stateless) -> chỉ uỷ thác. KHÔNG ghi file (UI tạo qua CollectionStore).
bool Engine::looksLikeCurl(const std::string& text) const { return CurlImporter{}.canHandle(text); }
bool Engine::looksLikeGrpcurl(const std::string& text) const { return GrpcImporter{}.canHandle(text); }
ImportResult Engine::importFromCurl(const std::string& text) const { return CurlImporter{}.parse(text); }
ImportResult Engine::importFromGrpc(const std::string& text) const { return GrpcImporter{}.parse(text); }

ValidationResult Engine::validateJson(const std::string& text) const {
    try {
        auto _ = nlohmann::json::parse(text);
        return ValidationResult{true, 0, 0, ""};
    } catch (const nlohmann::json::parse_error& e) {
        // e.byte -> line/col bằng cách đếm '\n' trước offset.
        int line = 1, col = 1;
        size_t limit = e.byte > 0 ? e.byte - 1 : 0;
        for (size_t i = 0; i < limit && i < text.size(); ++i) {
            if (text[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
        return ValidationResult{false, line, col, e.what()};
    }
}

std::string Engine::resolvePreview(const std::string& tpl) const {
    return VariableResolver::resolve(tpl, activeVars()).text;
}

} // namespace core
