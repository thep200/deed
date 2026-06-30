// core/cache.hpp — Two-tier response cache (RAM L1 -> Disk L2). RESPONSE_CACHE.md.
// Keyed by stable request `id`; each id keeps only the latest response. RAM is hard-capped
// per CacheConfig (clamped from env+user, built in Engine). Driver abstracted via ICacheDriver
// -> adding a backend (sqlite/mmap…) only needs an interface impl, NOT touching facade/Engine.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "core/domain/response/api_error.hpp"    // domain ErrorKind
#include "core/domain/response/api_response.hpp"  // domain ApiResponse

namespace core {

// Cache ceiling/floor at ENV layer (ops-level). 0 = "unset" -> fall back to getenv/default. The UI loads
// these from .env (DeedConfig) and passes them in; Core does not read .env itself (stays pure C++).
// (Lives here, not in the deleted engine.hpp, so detail::buildCacheConfig + CoreApiClient can use it.)
struct CacheLimits {
    int ramMaxMb = 0;
    int ramMinMb = 0;
    int diskMaxMb = 0;
    int diskMinMb = 0;
    int thresholdKb = 0;
};

// EFFECTIVE config (already clamped env/user) exposed to cache. Cache code does NOT read env/AppConfig.
struct CacheConfig {
    std::uint64_t ramEffBytes = 64ull * 1024 * 1024;   // effective RAM limit
    std::uint64_t diskEffBytes = 256ull * 1024 * 1024; // effective disk limit
    std::uint64_t thresholdBytes = 256ull * 1024;      // response < threshold -> prefer RAM
    bool enabled = true;   // off -> cache nothing
    bool persist = true;   // off -> RAM only (no L2 disk attached)
};

// Latest response record for one request. (Domain DTOs — REFACTOR_SPEC D: cache speaks domain.)
struct ResponseRecord {
    bool isError = false;                       // true -> store error (network/timeout/cancel) to replay
    domain::ErrorKind errorKind = domain::ErrorKind::Internal; // valid when isError
    std::string errorMessage;
    domain::ApiResponse response;               // valid when !isError
    std::int64_t receivedAt = 0;                // epoch ms (disk LRU)
    std::string requestRevision;                // request fingerprint at send time (changed -> "stale response" badge)
    std::uint64_t bytes = 0;                    // estimated size for cap accounting (computed if = 0)
};

// Estimate total bytes of one record (body + headers + meta) for cap accounting.
std::uint64_t estimateBytes(const ResponseRecord& r);

// --- Abstract driver (RESPONSE_CACHE.md §3). Thread-safe internally (own mutex). ---
class ICacheDriver {
public:
    virtual ~ICacheDriver() = default;
    virtual std::optional<ResponseRecord> get(const std::string& id) = 0;
    // false = rejected (e.g. single record > cap -> not cached at this tier).
    virtual bool put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) = 0;
    // rvalue overload: a driver that KEEPS a copy (e.g. RAM) can move instead of copy -> avoid copying large body.
    // Default delegates to the const& version (driver keeps no copy, e.g. disk, no need to override).
    virtual bool put(const std::string& id, ResponseRecord&& r, std::uint64_t bytes) {
        return put(id, static_cast<const ResponseRecord&>(r), bytes);
    }
    virtual void remove(const std::string& id) = 0;
    virtual void clear() = 0;
    virtual void setCapBytes(std::uint64_t cap) = 0;   // change cap at runtime -> evict if needed
    // Persist any deferred metadata (e.g. disk LRU atime). Default no-op (RAM keeps nothing on disk).
    // Called on shutdown because dtors don't run on macOS app terminate (Fix 2).
    virtual void flush() {}
    virtual std::uint64_t usedBytes() const = 0;
    virtual const char* name() const = 0;              // "ram" | "disk" | future…
};

// --- Facade L1(RAM) -> L2(Disk). Look up RAM first; write-through + prefer RAM when small. ---
class ResponseCache {
public:
    // l2 = nullptr if persist off (RAM only). Takes ownership of drivers -> allows injecting fake drivers (test/§10).
    ResponseCache(std::unique_ptr<ICacheDriver> l1,
                  std::unique_ptr<ICacheDriver> l2,
                  std::uint64_t ramThresholdBytes);
    ~ResponseCache();

    // Build a default cache from CacheConfig: RAM L1 + (persist ? Disk L2 : none).
    // sessionDir = collection's .session dir (disk stores under sessionDir/responses/).
    static std::unique_ptr<ResponseCache> create(const CacheConfig& cfg, const std::string& sessionDir);

    std::optional<ResponseRecord> get(const std::string& id);   // RAM first -> disk -> promote
    void put(const std::string& id, ResponseRecord r);          // write-through + prefer RAM
    void remove(const std::string& id);                         // remove from both tiers
    void clear();
    void flush();                                               // persist deferred metadata on both tiers (Fix 2)
    void onConfigChanged(const CacheConfig& c);                 // reload cap + threshold -> evict

    // Observability (test/diagnostic).
    std::uint64_t l1UsedBytes() const;
    std::uint64_t l2UsedBytes() const;

private:
    std::unique_ptr<ICacheDriver> l1_;
    std::unique_ptr<ICacheDriver> l2_;   // may be null (persist off)
    std::uint64_t ramThresholdBytes_;
};

} // namespace core
