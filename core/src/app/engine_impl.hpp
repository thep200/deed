// engine_impl.hpp — INTERNAL (core/src). Engine::Impl + the cache-config helpers it needs, shared by the
// Engine translation units (engine.cpp lifecycle/dispatch, engine_cache.cpp). Pulls heavy transport headers,
// so it is included ONLY by engine*.cpp — never from a public header (keeps grpc/curl out of the port).
#pragma once

#include "core/engine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core/cache.hpp"
#include "infra/fs_util.hpp"
#include "sending/graphql_sender.hpp"
#include "sending/grpc_sender.hpp"
#include "sending/http_sender.hpp"
#include "sending/sender_registry.hpp"
#include "sending/ws_sender.hpp"
#include "infra/thread_pool.hpp"

namespace core {

namespace detail {

// Read a numeric env var (>0) or fall back to default. ENV layer = hard ceiling (RESPONSE_CACHE §1).
inline std::uint64_t envU64(const char* key, std::uint64_t def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
    try { long long n = std::stoll(v); return n > 0 ? static_cast<std::uint64_t>(n) : def; }
    catch (...) { return def; }
}

// Take ceiling/floor from CacheLimits (.env loaded by UI) if set; otherwise fall back to getenv then default.
inline std::uint64_t limitOr(int fromEnvFile, const char* envKey, std::uint64_t def) {
    if (fromEnvFile > 0) return static_cast<std::uint64_t>(fromEnvFile);
    return envU64(envKey, def);
}

// effective = clamp(user, min, max); user outside [min,max] -> clamp + warning log (RESPONSE_CACHE §1.2).
inline CacheConfig buildCacheConfig(const AppConfig& app, const CacheLimits& lim) {
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

} // namespace detail

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
        registry.registerSender(RequestType::GraphQL, std::make_unique<GraphQlSender>());
        rebuildCache();
    }

    // (Re)build response cache from current AppConfig + the collection's .session dir.
    void rebuildCache() {
        std::lock_guard<std::mutex> lk(cacheMu);
        cacheCfg = detail::buildCacheConfig(appConfig.load(), cacheLimits);
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

} // namespace core
