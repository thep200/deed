// engine_cache.cpp — Engine's response-cache facade (split out of engine.cpp per SPEC_refactor §4.1).
// Shares Engine::Impl + detail::buildCacheConfig via engine_impl.hpp; behavior identical to the original.
#include "app/engine_impl.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace core {

namespace {
std::int64_t nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
// Request fingerprint at send time (change -> "stale response" badge). Light hash of resolvedRequestDump.
std::string revisionOf(const std::string& resolvedDump) {
    if (resolvedDump.empty()) return "";
    return std::to_string(std::hash<std::string>{}(resolvedDump));
}
} // namespace

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
        fresh = detail::buildCacheConfig(impl_->appConfig.load(), impl_->cacheLimits);
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

} // namespace core
