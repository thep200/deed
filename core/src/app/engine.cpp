#include "core/engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#include "core/cache.hpp"

#include "core/sending/i_request_sender.hpp"
#include "core/variables/variable_resolver.hpp"
#include "infra/fs_util.hpp"
#include "sending/grpc_descriptors.hpp"
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

// Đọc biến môi trường số (>0) hoặc default. ENV layer = trần cứng (RESPONSE_CACHE §1).
std::uint64_t envU64(const char* key, std::uint64_t def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
    try { long long n = std::stoll(v); return n > 0 ? static_cast<std::uint64_t>(n) : def; }
    catch (...) { return def; }
}

// Lấy trần/sàn từ CacheLimits (.env do UI nạp) nếu set; ngược lại fallback getenv rồi default.
std::uint64_t limitOr(int fromEnvFile, const char* envKey, std::uint64_t def) {
    if (fromEnvFile > 0) return static_cast<std::uint64_t>(fromEnvFile);
    return envU64(envKey, def);
}

// effective = clamp(user, min, max); user ngoài [min,max] -> kẹp + log cảnh báo (RESPONSE_CACHE §1.2).
CacheConfig buildCacheConfig(const AppConfig& app, const CacheLimits& lim) {
    std::uint64_t ramMaxMb = limitOr(lim.ramMaxMb, "DEED_RAM_CACHE_SIZE_MAX", 256);
    std::uint64_t ramMinMb = limitOr(lim.ramMinMb, "DEED_RAM_CACHE_SIZE_MIN", 0);
    std::uint64_t diskMaxMb = limitOr(lim.diskMaxMb, "DEED_DISK_CACHE_SIZE_MAX", 1024);
    std::uint64_t diskMinMb = limitOr(lim.diskMinMb, "DEED_DISK_CACHE_SIZE_MIN", 0);
    std::uint64_t thrKb = limitOr(lim.thresholdKb, "DEED_RAM_CACHE_THRESHOLD_KB", 256);

    // clamp(user, min, max) + log khi bị kẹp. min > max (misconfig) -> ưu tiên max làm trần.
    auto clampMb = [](const char* what, std::uint64_t user, std::uint64_t mn, std::uint64_t mx) {
        if (mn > mx) mn = mx;
        std::uint64_t v = user;
        if (v > mx) { v = mx; std::fprintf(stderr, "[cache] %s=%lluMB > max %lluMB -> kẹp về %lluMB\n",
                                           what, (unsigned long long)user, (unsigned long long)mx, (unsigned long long)mx); }
        if (v < mn) { std::fprintf(stderr, "[cache] %s=%lluMB < min %lluMB -> nâng lên %lluMB\n",
                                   what, (unsigned long long)v, (unsigned long long)mn, (unsigned long long)mn); v = mn; }
        return v;
    };

    // Đọc MỨC từ CẤU HÌNH NGƯỜI DÙNG (AppConfig) — đây là layer user (Settings).
    std::uint64_t ramUserMb = app.ramCacheSizeMb > 0 ? static_cast<std::uint64_t>(app.ramCacheSizeMb) : 0;
    std::uint64_t diskUserMb = app.diskCacheSizeMb > 0 ? static_cast<std::uint64_t>(app.diskCacheSizeMb) : 0;

    CacheConfig c;
    c.ramEffBytes = clampMb("ram_cache_size", ramUserMb, ramMinMb, ramMaxMb) * 1024ull * 1024ull;
    c.diskEffBytes = clampMb("disk_cache_size", diskUserMb, diskMinMb, diskMaxMb) * 1024ull * 1024ull;
    c.thresholdBytes = thrKb * 1024ull;
    c.enabled = app.cacheResponses;   // mặc định true (không phơi trong Settings)
    c.persist = app.cachePersist;     // mặc định true
    return c;
}

std::int64_t nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Dấu vết request lúc gửi (đổi -> badge "response cũ"). Hash nhẹ của resolvedRequestDump.
std::string revisionOf(const std::string& resolvedDump) {
    if (resolvedDump.empty()) return "";
    return std::to_string(std::hash<std::string>{}(resolvedDump));
}

} // namespace

struct Engine::Impl {
    explicit Impl(EngineConfig cfg)
        : collectionRoot(std::move(cfg.collectionRoot)),
          cacheLimits(cfg.cacheLimits),
          collection(collectionRoot),
          session(collectionRoot),
          secrets(std::make_shared<FileSecretStore>(collectionRoot)),
          environments(collectionRoot, secrets),
          appConfig(cfg.appConfigPath.empty() ? AppConfigStore()
                                              : AppConfigStore(cfg.appConfigPath)) {
        appConfig.setDefaults(cfg.appDefaults);   // defaults .env cho key thiếu trong config.json
        registry.registerSender(RequestType::Http, std::make_unique<HttpSender>());
        registry.registerSender(RequestType::Grpc, std::make_unique<GrpcSender>());
        rebuildCache();
    }

    // (Re)build response cache từ AppConfig hiện tại + thư mục .session của collection.
    void rebuildCache() {
        std::lock_guard<std::mutex> lk(cacheMu);
        cacheCfg = buildCacheConfig(appConfig.load(), cacheLimits);
        if (cacheCfg.enabled) {
            std::string sessionDir = fsutil::join(collectionRoot, ".session");
            cache = ResponseCache::create(cacheCfg, sessionDir);
        } else {
            cache.reset();   // tắt cache -> không giữ gì
        }
    }

    std::string collectionRoot;
    CacheLimits cacheLimits;                  // trần/sàn cache từ .env (qua EngineConfig)
    CollectionStore collection;
    SessionStore session;
    std::shared_ptr<FileSecretStore> secrets;
    EnvironmentStore environments;
    AppConfigStore appConfig;

    std::mutex cacheMu;                       // bảo vệ rebuild/đổi con trỏ cache (chỉ giữ khi copy con trỏ)
    CacheConfig cacheCfg;
    // shared_ptr: worker copy con trỏ ra NGOÀI lock rồi thao tác (I/O đĩa) -> cacheMu không bị
    // giữ suốt I/O; rebuild đổi con trỏ vẫn an toàn vì bản cũ sống tới khi worker xong (§1.3).
    std::shared_ptr<ResponseCache> cache;     // null khi tắt cache

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
    impl_->rebuildCache();        // disk cache dir đổi theo collection -> dựng lại
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

std::vector<GrpcMethodInfo> Engine::listGrpcMethods(const GrpcRequest& grpc,
                                                    std::string& error) const {
    // Resolve {{var}} cho target để reflection trỏ đúng host:port.
    GrpcRequest g = grpc;
    g.target = resolveStr(g.target, activeVars());

    grpcdesc::DescriptorContext ctx;
    if (!grpcdesc::buildDescriptors(g, ctx)) {
        error = ctx.error;
        return {};
    }
    return grpcdesc::listMethods(ctx);
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

// --- Response cache ---
// Mẫu chung: copy shared_ptr cache RA NGOÀI lock (giữ cacheMu chỉ trong ngoặc), rồi thao tác I/O
// trên bản copy. Bản cache cũ sống tới khi mọi shared_ptr thả -> rebuild song song an toàn (§1.3).
void Engine::putResponse(const std::string& id, const ApiResponse& resp) {
    if (id.empty()) return;
    std::shared_ptr<ResponseCache> c;
    { std::lock_guard<std::mutex> lk(impl_->cacheMu); c = impl_->cache; }
    if (!c) return;
    ResponseRecord rec;
    rec.isError = false;
    rec.response = resp;
    rec.receivedAt = nowEpochMs();
    rec.requestRevision = revisionOf(resp.resolvedRequestDump);
    c->put(id, std::move(rec));
}

void Engine::putError(const std::string& id, const ApiError& err) {
    if (id.empty()) return;
    std::shared_ptr<ResponseCache> c;
    { std::lock_guard<std::mutex> lk(impl_->cacheMu); c = impl_->cache; }
    if (!c) return;
    ResponseRecord rec;
    rec.isError = true;
    rec.errorKind = err.kind;
    rec.errorMessage = err.message;
    rec.receivedAt = nowEpochMs();
    c->put(id, std::move(rec));
}

std::optional<ResponseRecord> Engine::getResponse(const std::string& id) {
    std::shared_ptr<ResponseCache> c;
    { std::lock_guard<std::mutex> lk(impl_->cacheMu); c = impl_->cache; }
    if (!c) return std::nullopt;
    return c->get(id);
}

void Engine::removeResponse(const std::string& id) {
    std::shared_ptr<ResponseCache> c;
    { std::lock_guard<std::mutex> lk(impl_->cacheMu); c = impl_->cache; }
    if (c) c->remove(id);
}

void Engine::reloadCacheConfig() {
    std::shared_ptr<ResponseCache> c;
    CacheConfig fresh;
    {
        std::lock_guard<std::mutex> lk(impl_->cacheMu);
        bool wasPersist = impl_->cacheCfg.persist;
        bool wasEnabled = impl_->cacheCfg.enabled;
        fresh = buildCacheConfig(impl_->appConfig.load(), impl_->cacheLimits);
        // Đổi cap/threshold tại chỗ nếu cấu trúc tầng không đổi (giữ L1) -> evict ngay.
        if (impl_->cache && fresh.enabled == wasEnabled && fresh.persist == wasPersist) {
            impl_->cacheCfg = fresh;
            c = impl_->cache;            // copy con trỏ; gọi onConfigChanged NGOÀI lock (evict đĩa)
        }
    }
    if (c) { c->onConfigChanged(fresh); return; }
    // Bật/tắt cache hoặc đổi persist (gắn/tháo L2) -> dựng lại.
    impl_->rebuildCache();
}

const CacheConfig& Engine::cacheConfig() const { return impl_->cacheCfg; }
ResponseCache* Engine::responseCache() { return impl_->cache.get(); }

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
