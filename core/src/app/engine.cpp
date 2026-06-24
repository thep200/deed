#include "core/engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/cache.hpp"

#include "core/sending/i_request_sender.hpp"
#include "core/variables/variable_resolver.hpp"
#include "infra/fs_util.hpp"
#include "graphql/graphql.hpp"
#include "sending/grpc_descriptors.hpp"
#include "sending/grpc_sender.hpp"
#include "sending/http_sender.hpp"
#include "sending/ws_sender.hpp"
#include "codec/json_codec.hpp"
#include "sending/sender_registry.hpp"
#include "infra/thread_pool.hpp"

namespace fs = std::filesystem;

namespace core {

namespace {

// Resolve {{var}} for a KeyValue array (value field).
void resolveKv(std::vector<KeyValue>& kvs, const std::map<std::string, std::string>& vars) {
    for (auto& kv : kvs) kv.value = VariableResolver::resolve(kv.value, vars).text;
}

std::string resolveStr(const std::string& s, const std::map<std::string, std::string>& vars) {
    return VariableResolver::resolve(s, vars).text;
}

// Read a numeric env var (>0) or fall back to default. ENV layer = hard ceiling (RESPONSE_CACHE §1).
std::uint64_t envU64(const char* key, std::uint64_t def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
    try { long long n = std::stoll(v); return n > 0 ? static_cast<std::uint64_t>(n) : def; }
    catch (...) { return def; }
}

// Take ceiling/floor from CacheLimits (.env loaded by UI) if set; otherwise fall back to getenv then default.
std::uint64_t limitOr(int fromEnvFile, const char* envKey, std::uint64_t def) {
    if (fromEnvFile > 0) return static_cast<std::uint64_t>(fromEnvFile);
    return envU64(envKey, def);
}

// effective = clamp(user, min, max); user outside [min,max] -> clamp + warning log (RESPONSE_CACHE §1.2).
CacheConfig buildCacheConfig(const AppConfig& app, const CacheLimits& lim) {
    std::uint64_t ramMaxMb = limitOr(lim.ramMaxMb, "DEED_RAM_CACHE_SIZE_MAX", 256);
    std::uint64_t ramMinMb = limitOr(lim.ramMinMb, "DEED_RAM_CACHE_SIZE_MIN", 0);
    std::uint64_t diskMaxMb = limitOr(lim.diskMaxMb, "DEED_DISK_CACHE_SIZE_MAX", 1024);
    std::uint64_t diskMinMb = limitOr(lim.diskMinMb, "DEED_DISK_CACHE_SIZE_MIN", 0);
    std::uint64_t thrKb = limitOr(lim.thresholdKb, "DEED_RAM_CACHE_THRESHOLD_KB", 256);

    // clamp(user, min, max) + log when clamped. min > max (misconfig) -> prefer max as ceiling.
    auto clampMb = [](const char* what, std::uint64_t user, std::uint64_t mn, std::uint64_t mx) {
        if (mn > mx) mn = mx;
        std::uint64_t v = user;
        if (v > mx) { v = mx; std::fprintf(stderr, "[cache] %s=%lluMB > max %lluMB -> clamped to %lluMB\n",
                                           what, (unsigned long long)user, (unsigned long long)mx, (unsigned long long)mx); }
        if (v < mn) { std::fprintf(stderr, "[cache] %s=%lluMB < min %lluMB -> raised to %lluMB\n",
                                   what, (unsigned long long)v, (unsigned long long)mn, (unsigned long long)mn); v = mn; }
        return v;
    };

    // Read the LEVEL from the USER CONFIG (AppConfig) — this is the user layer (Settings).
    std::uint64_t ramUserMb = app.ramCacheSizeMb > 0 ? static_cast<std::uint64_t>(app.ramCacheSizeMb) : 0;
    std::uint64_t diskUserMb = app.diskCacheSizeMb > 0 ? static_cast<std::uint64_t>(app.diskCacheSizeMb) : 0;

    CacheConfig c;
    c.ramEffBytes = clampMb("ram_cache_size", ramUserMb, ramMinMb, ramMaxMb) * 1024ull * 1024ull;
    c.diskEffBytes = clampMb("disk_cache_size", diskUserMb, diskMinMb, diskMaxMb) * 1024ull * 1024ull;
    c.thresholdBytes = thrKb * 1024ull;
    c.enabled = app.cacheResponses;   // default true (not exposed in Settings)
    c.persist = app.cachePersist;     // default true
    return c;
}

std::int64_t nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Request fingerprint at send time (change -> "stale response" badge). Light hash of resolvedRequestDump.
std::string revisionOf(const std::string& resolvedDump) {
    if (resolvedDump.empty()) return "";
    return std::to_string(std::hash<std::string>{}(resolvedDump));
}

// Neutral transport for StreamMeta on setup-failure paths (M8: don't hard-code gRPC). Display/telemetry only.
StreamTransport transportOf(RequestType t) {
    switch (t) {
        case RequestType::WebSocket: return StreamTransport::WebSocket;
        case RequestType::Http:      return StreamTransport::Sse;   // HTTP stream path is SSE
        case RequestType::GraphQL:   return StreamTransport::Sse;   // subscription over WS/SSE
        default:                     return StreamTransport::Grpc;
    }
}

} // namespace

struct Engine::Impl {
    explicit Impl(EngineConfig cfg)
        : collectionRoot(std::move(cfg.collectionRoot)),
          cacheLimits(cfg.cacheLimits),
          streamLimits(cfg.streamLimits),
          wsLimits(cfg.wsLimits),
          collection(collectionRoot),
          session(collectionRoot),
          environments(collectionRoot),
          appConfig(cfg.appConfigPath.empty() ? AppConfigStore()
                                              : AppConfigStore(cfg.appConfigPath)) {
        appConfig.setDefaults(cfg.appDefaults);   // .env defaults for keys missing in config.json
        environments.migrateLegacySecrets();      // SPEC §T5: merge .secrets/ -> plaintext (one-time)
        registry.registerSender(RequestType::Http, std::make_unique<HttpSender>());
        registry.registerSender(RequestType::Grpc, std::make_unique<GrpcSender>());
        rebuildCache();
    }

    // (Re)build response cache from current AppConfig + the collection's .session dir.
    void rebuildCache() {
        std::lock_guard<std::mutex> lk(cacheMu);
        cacheCfg = buildCacheConfig(appConfig.load(), cacheLimits);
        if (cacheCfg.enabled) {
            std::string sessionDir = fsutil::join(collectionRoot, ".session");
            cache = ResponseCache::create(cacheCfg, sessionDir);
        } else {
            cache.reset();   // cache off -> hold nothing
        }
    }

    std::string collectionRoot;
    CacheLimits cacheLimits;                  // cache ceiling/floor from .env (via EngineConfig)
    StreamLimits streamLimits;                // stream ceilings from .env (via EngineConfig)
    WsLimits wsLimits;                        // WebSocket tunables from .env (via EngineConfig)
    CollectionStore collection;
    SessionStore session;
    EnvironmentStore environments;
    AppConfigStore appConfig;

    std::mutex cacheMu;                       // guards rebuild/swap of cache pointer (held only while copying the pointer)
    CacheConfig cacheCfg;
    // shared_ptr: worker copies the pointer OUTSIDE the lock then does its work (disk I/O) -> cacheMu is not
    // held across I/O; swapping the pointer on rebuild stays safe since the old instance lives until the worker is done (§1.3).
    std::shared_ptr<ResponseCache> cache;     // null when cache is off

    SenderRegistry registry;

    // Cache merged-vars (Global + active env) -> avoid reading/parsing env from disk on EVERY resolve/send.
    // Keyed by (activeEnv, env-epoch): switching the active env or editing any env -> epoch/key changes
    // -> auto-rebuild (never reuse stale vars). activeVars may run on main (send) and background
    // (listGrpcMethods via reflection) -> guarded by varsMu.
    std::mutex varsMu;
    std::shared_ptr<const std::map<std::string, std::string>> varsCache;   // immutable snapshot (M2)
    std::string varsCacheEnv;
    std::uint64_t varsCacheEpoch = 0;
    bool varsCacheValid = false;

    std::atomic<RequestHandle> nextHandle{1};

    // Inflight registry split into a shared_ptr object: worker keeps its own reference and
    // does NOT deref this->impl_ (libc++ nulls the unique_ptr before running ~Impl, while ~Impl
    // blocks on pool.join() -> worker reads null impl_ -> crash). Holding via shared_ptr is safe.
    struct InflightReg {
        std::mutex mu;
        std::map<RequestHandle, std::shared_ptr<CancelToken>> map;
    };
    std::shared_ptr<InflightReg> inflight = std::make_shared<InflightReg>();

    // Stream registry (SPEC_grpc_streaming §4/§7): streamId -> CancelToken, for cancel + lifecycle.
    // Same shared_ptr discipline as inflight (worker keeps its own reference; never derefs impl_).
    struct StreamReg {
        std::mutex mu;
        std::map<std::string, std::shared_ptr<CancelToken>> map;
    };
    std::shared_ptr<StreamReg> streams = std::make_shared<StreamReg>();
    std::atomic<std::uint64_t> nextStreamId{1};

    // Duplex session registry (SPEC_websocket §4): sessionId -> shared WsSession (for close + cleanup).
    struct SessionReg {
        std::mutex mu;
        std::map<std::string, std::shared_ptr<WsSession>> map;
    };
    std::shared_ptr<SessionReg> sessions = std::make_shared<SessionReg>();
    std::atomic<std::uint64_t> nextSessionId{1};

    // Drain barrier for detached stream/session threads (H1b/M6). Streams & sessions run on their OWN
    // threads (not the bounded pool) so a few long-lived streams can't starve unary sends, and a wedged
    // stream can't block the pool join. Each tracked thread ++active on start, --active + notify on finish;
    // ~Impl cancels everything then waits for active==0. Held by shared_ptr so detached threads that
    // outlast the backstop still decrement safely (they capture this, never impl_).
    struct Runners {
        std::mutex mu;
        std::condition_variable cv;
        int active = 0;
        bool draining = false;   // shutdown started -> reject new spawns
    };
    std::shared_ptr<Runners> runners = std::make_shared<Runners>();

    // Spawn a detached, drain-tracked worker. false -> shutting down (caller delivers an error instead).
    bool spawnTracked(std::function<void()> fn) {
        auto r = runners;
        {
            std::lock_guard<std::mutex> lk(r->mu);
            if (r->draining) return false;
            ++r->active;
        }
        std::thread([r, fn = std::move(fn)]() mutable {
            fn();
            std::lock_guard<std::mutex> lk(r->mu);
            if (--r->active == 0) r->cv.notify_all();
        }).detach();
        return true;
    }

    // (M6/H1c) Stop all in-flight work BEFORE members destruct: cancel unary sends + streams, close
    // sessions, then wait for the detached stream/session threads to drain. The pool (declared last)
    // destructs after this body and joins its unary-send workers, which exit promptly once cancelled.
    ~Impl() {
        { std::lock_guard<std::mutex> lk(inflight->mu); for (auto& kv : inflight->map) if (kv.second) kv.second->cancel(); }
        { std::lock_guard<std::mutex> lk(streams->mu);  for (auto& kv : streams->map)  if (kv.second) kv.second->cancel(); }
        { std::lock_guard<std::mutex> lk(sessions->mu); for (auto& kv : sessions->map) if (kv.second) wsRequestClose(kv.second, 1001, "shutdown"); }
        std::unique_lock<std::mutex> lk(runners->mu);
        runners->draining = true;
        // Backstop: a detached thread stuck in a blocking handshake is safe to outlive us (it captures
        // only shared_ptrs), so we don't wait forever.
        runners->cv.wait_for(lk, std::chrono::seconds(10), [this] { return runners->active == 0; });
    }

    // pool DECLARED LAST -> destructor runs FIRST (reverse order): join all workers
    // before registry/inflight are destroyed, ensuring senders stay alive throughout sending.
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
    impl_->environments.setRoot(root);
    impl_->environments.migrateLegacySecrets(); // new collection may still have an old .secrets/
    impl_->collection.ensureGitignore();
    impl_->rebuildCache();        // disk cache dir changes with collection -> rebuild
}

CollectionStore& Engine::collection() { return impl_->collection; }
SessionStore& Engine::session() { return impl_->session; }
EnvironmentStore& Engine::environments() { return impl_->environments; }
AppConfigStore& Engine::appConfig() { return impl_->appConfig; }

std::shared_ptr<const std::map<std::string, std::string>> Engine::activeVarsSnapshot() const {
    std::string active = impl_->session.getActiveEnv();
    std::uint64_t epoch = impl_->environments.epoch();
    {   // Cache hit: (active env + epoch) unchanged -> share the snapshot pointer, NO copy, NO disk read.
        std::lock_guard<std::mutex> lk(impl_->varsMu);
        if (impl_->varsCacheValid && impl_->varsCacheEnv == active && impl_->varsCacheEpoch == epoch)
            return impl_->varsCache;
    }
    // Miss: rebuild OUTSIDE the lock (read + parse env from disk).
    auto vars = std::make_shared<std::map<std::string, std::string>>();
    auto merge = [&](const std::string& name) {
        try {
            Environment e = impl_->environments.load(name);
            for (const auto& k : e.keys)
                if (k.enabled) (*vars)[k.key] = k.value;
        } catch (...) { /* env does not exist -> skip */ }
    };
    merge("Global");                         // base
    if (!active.empty() && active != "Global") merge(active); // override
    std::shared_ptr<const std::map<std::string, std::string>> snap = vars;
    {
        std::lock_guard<std::mutex> lk(impl_->varsMu);
        impl_->varsCache = snap;
        impl_->varsCacheEnv = active;
        impl_->varsCacheEpoch = epoch;
        impl_->varsCacheValid = true;
    }
    return snap;
}

std::map<std::string, std::string> Engine::activeVars() const {
    return *activeVarsSnapshot();   // public API: one copy (callers that need a mutable map)
}

std::vector<std::pair<std::string, std::string>> Engine::activeVarsOrdered() const {
    std::string active = impl_->session.getActiveEnv();
    std::vector<std::pair<std::string, std::string>> vars;
    auto merge = [&](const std::string& name) {
        try {
            Environment e = impl_->environments.load(name);
            for (const auto& k : e.keys) {
                if (!k.enabled) continue;
                // Same key in a later env -> override value, keep first position (active wins value).
                auto it = std::find_if(vars.begin(), vars.end(),
                                       [&](const auto& p) { return p.first == k.key; });
                if (it != vars.end()) it->second = k.value;
                else vars.emplace_back(k.key, k.value);
            }
        } catch (...) { /* env does not exist -> skip */ }
    };
    merge("Global");                         // base
    if (!active.empty() && active != "Global") merge(active); // override
    return vars;
}

// Map the unified per-request config (timeout + TLS) onto the active transport's settings.
// HTTP/GraphQL: timeout + verify-TLS. gRPC: deadline + TLS-channel-enabled. (WS handled in openSession.)
static void applyRequestConfig(RequestModel& m) {
    const RequestConfig& c = m.config;
    if (m.type == RequestType::Http) {
        m.http.settings.timeoutMs = c.timeoutMs;   m.http.settings.timeoutMsSet = true;
        m.http.settings.verifyTls = c.tls;         m.http.settings.verifyTlsSet = true;
    } else if (m.type == RequestType::Grpc) {
        m.grpc.settings.deadlineMs = c.timeoutMs;  m.grpc.settings.deadlineMsSet = true;
        m.grpc.tls.enabled = c.tls;
    }
}

ResolvedRequest Engine::resolveRequest(const RequestModel& model) const {
    auto varsPtr = activeVarsSnapshot();   // shared snapshot, no per-send map copy (M2)
    const auto& vars = *varsPtr;
    ResolvedRequest rr;
    rr.model = model;

    // --- Timeout + TLS come from the per-request Config tab (RequestConfig). ---
    applyRequestConfig(rr.model);

    // --- Resolve {{var}} in all string fields ---
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
    } else if (rr.model.type == RequestType::WebSocket) {
        auto& ws = rr.model.ws;
        ws.url = resolveStr(ws.url, vars);
        resolveKv(ws.headers, vars);
        for (auto& m : ws.onOpenSend) m = resolveStr(m, vars);
        ws.auth.bearerToken = resolveStr(ws.auth.bearerToken, vars);
        ws.auth.basicUsername = resolveStr(ws.auth.basicUsername, vars);
        ws.auth.basicPassword = resolveStr(ws.auth.basicPassword, vars);
        ws.auth.apikeyValue = resolveStr(ws.auth.apikeyValue, vars);
    } else if (rr.model.type == RequestType::GraphQL) {
        auto& g = rr.model.graphql;
        g.url = resolveStr(g.url, vars);
        resolveKv(g.headers, vars);
        g.variablesJson = resolveStr(g.variablesJson, vars);
        g.connectionInitPayloadJson = resolveStr(g.connectionInitPayloadJson, vars);
        g.auth.bearerToken = resolveStr(g.auth.bearerToken, vars);
        g.auth.basicUsername = resolveStr(g.auth.basicUsername, vars);
        g.auth.basicPassword = resolveStr(g.auth.basicPassword, vars);
        g.auth.apikeyValue = resolveStr(g.auth.apikeyValue, vars);
    } else {
        auto& g = rr.model.grpc;
        g.target = resolveStr(g.target, vars);
        g.message = resolveStr(g.message, vars);
        resolveKv(g.metadata, vars);
    }
    return rr;
}

std::vector<std::string> Engine::missingVars(const RequestModel& model) const {
    auto varsPtr = activeVarsSnapshot();   // shared snapshot (M2)
    const auto& vars = *varsPtr;
    std::vector<std::string> miss;
    std::set<std::string> seen;
    auto scan = [&](const std::string& s) {
        for (auto& m : VariableResolver::resolve(s, vars).missing)
            if (seen.insert(m).second) miss.push_back(m);
    };
    auto scanKv = [&](const std::vector<KeyValue>& kvs) {
        for (auto& kv : kvs) if (kv.enabled) scan(kv.value);  // disabled lines aren't sent
    };
    if (model.type == RequestType::Http) {
        const auto& h = model.http;
        scan(h.url);
        scanKv(h.pathVariables);
        scanKv(h.params);
        scanKv(h.headers);
        scan(h.auth.bearerToken);
        scan(h.auth.basicUsername);
        scan(h.auth.basicPassword);
        scan(h.auth.apikeyValue);
    } else {
        const auto& g = model.grpc;
        scan(g.target);
        scanKv(g.metadata);
    }
    return miss;
}

RequestModel Engine::aliasifyModel(const RequestModel& model,
                                   std::vector<std::string>* applied) const {
    auto vars = activeVarsOrdered();   // ordered -> first-defined key wins on duplicate values
    RequestModel m = model;
    std::set<std::string> appliedSet;
    auto whole = [&](std::string& s) {
        std::string out, key;
        if (VariableResolver::valueToAlias(s, vars, out, &key)) { s = out; appliedSet.insert(key); }
    };
    auto prefix = [&](std::string& s) {
        std::string out, key;
        if (VariableResolver::prefixToAlias(s, vars, out, &key)) { s = out; appliedSet.insert(key); }
    };
    auto wholeKv = [&](std::vector<KeyValue>& kvs) {
        for (auto& kv : kvs) if (kv.enabled) whole(kv.value);
    };
    auto aliasAuth = [&](Auth& a) {
        whole(a.bearerToken);
        whole(a.basicUsername);
        whole(a.basicPassword);
        whole(a.apikeyValue);
    };
    if (m.type == RequestType::Http) {
        auto& h = m.http;
        prefix(h.url);              // url: baseUrl-style prefix
        wholeKv(h.pathVariables);
        wholeKv(h.params);          // query
        wholeKv(h.headers);         // header
        aliasAuth(h.auth);          // auth (fields hold the bare secret -> exact match)
    } else if (m.type == RequestType::WebSocket) {
        auto& w = m.ws;
        prefix(w.url);
        wholeKv(w.headers);
        aliasAuth(w.auth);
    } else if (m.type == RequestType::GraphQL) {
        auto& g = m.graphql;
        prefix(g.url);
        wholeKv(g.headers);
        aliasAuth(g.auth);
    } else {
        auto& g = m.grpc;
        prefix(g.target);
        wholeKv(g.metadata);
    }
    if (applied) applied->assign(appliedSet.begin(), appliedSet.end());
    return m;
}

std::vector<GrpcMethodInfo> Engine::listGrpcMethods(const GrpcRequest& grpc,
                                                    std::string& error) const {
    // Resolve {{var}} for target so reflection points to the right host:port.
    GrpcRequest g = grpc;
    g.target = resolveStr(g.target, activeVars());

    grpcdesc::DescriptorContext ctx;
    if (!grpcdesc::buildDescriptors(g, ctx)) {
        error = ctx.error;
        return {};
    }
    return grpcdesc::listMethods(ctx);
}

RequestHandle Engine::send(const RequestModel& model, std::shared_ptr<IUiDelegate> delegate) {
    RequestHandle handle = impl_->nextHandle.fetch_add(1);
    auto cancel = std::make_shared<CancelToken>();
    auto inflight = impl_->inflight; // shared_ptr — worker keeps its own reference
    {
        std::lock_guard<std::mutex> lk(inflight->mu);
        inflight->map[handle] = cancel;
    }

    // Resolve + look up sender NOW on the calling thread (Engine still alive) -> worker won't deref impl_.
    // GraphQL query/mutation is repackaged into an HTTP POST (reuse HttpSender) AFTER {{var}} resolution
    // so the variables JSON parses cleanly (SPEC_graphql §4).
    IRequestSender* sender = nullptr;
    std::shared_ptr<ResolvedRequest> rr;
    std::shared_ptr<ApiError> immediateError;
    try {
        rr = std::make_shared<ResolvedRequest>(resolveRequest(model));
        if (model.type == RequestType::GraphQL &&
            gql::effectiveOperation(model.graphql) != GqlOperation::Subscription) {
            rr->model = gql::buildHttpModel(rr->model);
            applyRequestConfig(rr->model);   // buildHttpModel resets http settings -> re-apply config
        }
        sender = impl_->registry.get(rr->model.type);
        if (!sender)
            immediateError = std::make_shared<ApiError>(
                ApiError{ErrorKind::Unsupported, "no sender for this request type"});
    } catch (const std::exception& e) {
        immediateError = std::make_shared<ApiError>(ApiError{ErrorKind::Unknown, e.what()});
    }

    // Worker only touches captured vars: the delegate is held by shared_ptr (C1 — stays alive across the
    // whole call even if the owning window closes), inflight is a shared_ptr, the sender outlives the worker
    // (drained/joined in ~Impl before the registry destructs).
    auto task = [handle, delegate, cancel, inflight, sender, rr, immediateError]() {
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
    };
    // Prefer the bounded pool; if it's full (or shutting down) fall back to a dedicated tracked thread so
    // the send still runs and stays drainable at shutdown.
    if (!impl_->pool.submit(task)) impl_->spawnTracked(std::move(task));
    return handle;
}

void Engine::cancel(RequestHandle handle) {
    auto inflight = impl_->inflight;
    std::lock_guard<std::mutex> lk(inflight->mu);
    auto it = inflight->map.find(handle);
    if (it != inflight->map.end()) it->second->cancel();
}

// --- Server-streaming (SPEC_grpc_streaming §4) ---

InteractionKind Engine::interactionOf(const RequestModel& model) const {
    // gRPC: derived from the method descriptor's type (set when the RPC was picked — §4, no round-trip).
    if (model.type == RequestType::WebSocket) return InteractionKind::Duplex;
    // GraphQL (SPEC_graphql §3): subscription -> stream (openStream); query/mutation -> unary (send).
    if (model.type == RequestType::GraphQL)
        return gql::effectiveOperation(model.graphql) == GqlOperation::Subscription
                   ? InteractionKind::ServerStream : InteractionKind::Unary;
    // SSE (SPEC_sse §5): HTTP consumes the response as a stream when streamMode is Sse/Auto OR the
    // request carries an `Accept: text/event-stream` header -> reuse openStream.
    if (model.type == RequestType::Http && httpRequestsSse(model.http))
        return InteractionKind::ServerStream;
    if (model.type == RequestType::Grpc) {
        if (model.grpc.methodType == "server_streaming") return InteractionKind::ServerStream;
        if (model.grpc.methodType == "client_streaming") return InteractionKind::ClientStream;
        if (model.grpc.methodType == "bidi_streaming") return InteractionKind::BiDi;
    }
    // Routing: ServerStream + BiDi produce a STREAM of responses -> openStream()/IStreamSink. ClientStream
    // yields ONE response -> unary send() path. The UI/CLI route accordingly.
    return InteractionKind::Unary;
}

StreamHandle Engine::openStream(const RequestModel& model, std::shared_ptr<IStreamSink> sink) {
    StreamHandle h;
    if (!sink) return h;
    h.streamId = "stream-" + std::to_string(impl_->nextStreamId.fetch_add(1));
    auto cancel = std::make_shared<CancelToken>();
    auto streams = impl_->streams;  // shared_ptr — worker keeps its own reference
    {
        std::lock_guard<std::mutex> lk(streams->mu);
        streams->map[h.streamId] = cancel;
    }

    // Resolve + look up sender NOW on the calling thread (Engine still alive) — same discipline as send().
    IRequestSender* sender = impl_->registry.get(model.type);
    std::shared_ptr<ResolvedRequest> rr;
    std::shared_ptr<std::string> immediateError;
    if (!sender) {
        immediateError = std::make_shared<std::string>("no sender for this request type");
    } else {
        try {
            rr = std::make_shared<ResolvedRequest>(resolveRequest(model));
            rr->streamId = h.streamId;   // sender stamps StreamMeta with this
            rr->streamMaxEvents = impl_->streamLimits.maxEvents;   // §9 ceilings from .env (0 -> sender default)
            rr->streamMaxBytes = impl_->streamLimits.maxBytes;
        } catch (const std::exception& e) {
            immediateError = std::make_shared<std::string>(e.what());
        }
    }

    const std::string sid = h.streamId;
    const StreamTransport transport = transportOf(model.type);   // M8: report the real transport, not always gRPC
    // Streams run on a DEDICATED thread (H1b): a long stream no longer pins a pool worker, and the sink is
    // held by shared_ptr (C1) so it can't be freed under the worker. Drained at shutdown (M6).
    auto task = [sid, sink, cancel, streams, sender, rr, immediateError, transport]() {
        try {
            if (immediateError) {
                // Honor the §3 contract even on a setup failure: open then close(Error).
                sink->onStreamOpen(StreamMeta{sid, transport, {}, 0});
                StreamEnd end;
                end.status = StreamStatus::Error;
                end.statusMessage = *immediateError;
                sink->onStreamClose(end);
            } else {
                sender->openStream(*rr, *sink, cancel);
            }
        } catch (const std::exception& e) {
            StreamEnd end;
            end.status = StreamStatus::Error;
            end.statusMessage = e.what();
            sink->onStreamClose(end);   // sender already emitted onStreamOpen before any throw
        }
        std::lock_guard<std::mutex> lk(streams->mu);
        streams->map.erase(sid);
    };
    if (!impl_->spawnTracked(std::move(task))) {
        // Shutting down — honor the §3 contract synchronously so the caller still sees open+close.
        sink->onStreamOpen(StreamMeta{sid, transport, {}, 0});
        StreamEnd end; end.status = StreamStatus::Error; end.statusMessage = "engine shutting down";
        sink->onStreamClose(end);
        std::lock_guard<std::mutex> lk(streams->mu);
        streams->map.erase(sid);
    }
    return h;
}

void Engine::cancelStream(const StreamHandle& h) {
    if (h.streamId.empty()) return;
    auto streams = impl_->streams;
    std::lock_guard<std::mutex> lk(streams->mu);
    auto it = streams->map.find(h.streamId);
    if (it != streams->map.end()) it->second->cancel();   // hook + flag -> sender stops its Read loop
}

// --- Duplex session (SPEC_websocket §4) ---

SessionHandle Engine::openSession(const RequestModel& model, std::shared_ptr<IStreamSink> inbound) {
    SessionHandle h;
    if (!inbound) return h;

    // Build WS config: .env limits (0 -> WsSender default) + TLS verify from AppConfig.
    WsConfig cfg;
    const WsLimits& wl = impl_->wsLimits;
    if (wl.pingIntervalMs > 0) cfg.pingIntervalMs = wl.pingIntervalMs;
    if (wl.idleTimeoutMs > 0) cfg.idleTimeoutMs = wl.idleTimeoutMs;
    if (wl.closeTimeoutMs > 0) cfg.closeTimeoutMs = wl.closeTimeoutMs;
    if (wl.maxFrameBytes > 0) cfg.maxFrameBytes = static_cast<std::uint64_t>(wl.maxFrameBytes);
    if (wl.sendQueueMaxFrames > 0) cfg.sendQueueMaxFrames = static_cast<std::size_t>(wl.sendQueueMaxFrames);
    if (wl.sendQueueMaxBytes > 0) cfg.sendQueueMaxBytes = static_cast<std::uint64_t>(wl.sendQueueMaxBytes);
    // Per-request Config tab drives TLS verify + idle timeout for this WebSocket.
    cfg.verifyTls = model.config.tls;
    if (model.config.timeoutMs > 0) cfg.idleTimeoutMs = model.config.timeoutMs;

    h.sessionId = "ws-" + std::to_string(impl_->nextSessionId.fetch_add(1));
    auto session = wsMakeSession(cfg);
    h.channel = wsMakeChannel(session);

    auto sessions = impl_->sessions;
    { std::lock_guard<std::mutex> lk(sessions->mu); sessions->map[h.sessionId] = session; }

    std::shared_ptr<ResolvedRequest> rr;
    std::shared_ptr<std::string> immediateError;
    try {
        rr = std::make_shared<ResolvedRequest>(resolveRequest(model));
    } catch (const std::exception& e) {
        immediateError = std::make_shared<std::string>(e.what());
    }

    const std::string sid = h.sessionId;
    // Sessions run on a DEDICATED thread (H1b) and the sink is held by shared_ptr (C1). On shutdown ~Impl
    // calls wsRequestClose on every live session then drains these threads (H2/M6).
    auto task = [rr, inbound, session, sid, sessions, immediateError]() {
        try {
            if (immediateError) {
                inbound->onStreamOpen(StreamMeta{sid, StreamTransport::WebSocket, {}, 0});
                StreamEnd end;
                end.status = StreamStatus::Error;
                end.statusMessage = *immediateError;
                inbound->onStreamClose(end);
            } else {
                wsRun(*rr, *inbound, session, sid);
            }
        } catch (const std::exception& e) {
            StreamEnd end;
            end.status = StreamStatus::Error;
            end.statusMessage = e.what();
            inbound->onStreamClose(end);
        }
        std::lock_guard<std::mutex> lk(sessions->mu);
        sessions->map.erase(sid);
    };
    if (!impl_->spawnTracked(std::move(task))) {
        inbound->onStreamOpen(StreamMeta{sid, StreamTransport::WebSocket, {}, 0});
        StreamEnd end; end.status = StreamStatus::Error; end.statusMessage = "engine shutting down";
        inbound->onStreamClose(end);
        std::lock_guard<std::mutex> lk(sessions->mu);
        sessions->map.erase(sid);
    }
    return h;
}

void Engine::closeSession(const SessionHandle& h, int code, const std::string& reason) {
    if (h.sessionId.empty()) return;
    auto sessions = impl_->sessions;
    std::shared_ptr<WsSession> s;
    {
        std::lock_guard<std::mutex> lk(sessions->mu);
        auto it = sessions->map.find(h.sessionId);
        if (it != sessions->map.end()) s = it->second;
    }
    if (s) wsRequestClose(s, code, reason);
}

// --- Response cache ---
// Common pattern: copy the cache shared_ptr OUTSIDE the lock (hold cacheMu only in the braces), then do I/O
// on the copy. The old cache instance lives until all shared_ptrs are released -> concurrent rebuild is safe (§1.3).
void Engine::putResponse(const std::string& id, ApiResponse&& resp) {
    if (id.empty()) return;
    std::shared_ptr<ResponseCache> c;
    { std::lock_guard<std::mutex> lk(impl_->cacheMu); c = impl_->cache; }
    if (!c) return;
    ResponseRecord rec;
    rec.isError = false;
    rec.requestRevision = revisionOf(resp.resolvedRequestDump);   // read before the move
    rec.response = std::move(resp);                               // move the (possibly large) body (M3)
    rec.receivedAt = nowEpochMs();
    c->put(id, std::move(rec));
}

void Engine::putResponse(const std::string& id, const ApiResponse& resp) {
    putResponse(id, ApiResponse(resp));   // single copy here; the move overload owns the hot path
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
        // Change cap/threshold in place if tier structure is unchanged (keep L1) -> evict immediately.
        if (impl_->cache && fresh.enabled == wasEnabled && fresh.persist == wasPersist) {
            impl_->cacheCfg = fresh;
            c = impl_->cache;            // copy the pointer; call onConfigChanged OUTSIDE the lock (disk evict)
        }
    }
    // M5: onConfigChanged (cap + threshold) runs OUTSIDE the lock on purpose — it does disk eviction and
    // must not hold cacheMu across I/O. The brief window where a get/put sees the new threshold but the
    // driver still has the old cap is benign (eventual consistency) and accepted.
    if (c) { c->onConfigChanged(fresh); return; }
    // Toggle cache or change persist (attach/detach L2) -> rebuild.
    impl_->rebuildCache();
}

CacheConfig Engine::cacheConfig() const {
    // Return BY VALUE under the lock (M4): handing out a reference let a concurrent reloadCacheConfig/
    // rebuildCache mutation tear the struct under a reader.
    std::lock_guard<std::mutex> lk(impl_->cacheMu);
    return impl_->cacheCfg;
}
ResponseCache* Engine::responseCache() { return impl_->cache.get(); }

// Import: pure (stateless) importers -> just delegate. Does NOT write files (UI creates via CollectionStore).
bool Engine::looksLikeCurl(const std::string& text) const { return CurlImporter{}.canHandle(text); }
bool Engine::looksLikeGrpcurl(const std::string& text) const { return GrpcImporter{}.canHandle(text); }
bool Engine::looksLikeGraphql(const std::string& text) const { return GraphQlImporter{}.canHandle(text); }
ImportResult Engine::importFromCurl(const std::string& text) const { return CurlImporter{}.parse(text); }
ImportResult Engine::importFromGrpc(const std::string& text) const { return GrpcImporter{}.parse(text); }
ImportResult Engine::importFromGraphql(const std::string& text) const { return GraphQlImporter{}.parse(text); }

ValidationResult Engine::validateJson(const std::string& text) const {
    try {
        auto _ = nlohmann::json::parse(text);
        return ValidationResult{true, 0, 0, ""};
    } catch (const nlohmann::json::parse_error& e) {
        // e.byte -> line/col by counting '\n' before the offset.
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
