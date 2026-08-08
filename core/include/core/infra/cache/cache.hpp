#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "core/domain/response/api_error.hpp"
#include "core/domain/response/api_response.hpp"

namespace core {

// ENV-layer ceiling/floor; 0 = "unset" -> fall back to default. Passed in by the UI — Core never reads .env.
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

// Latest response record for one request.
struct ResponseRecord {
    bool isError = false;                       // true -> store error (network/timeout/cancel) to replay
    domain::ErrorKind errorKind = domain::ErrorKind::Internal; // valid when isError
    std::string errorMessage;
    domain::ApiResponse response;               // valid when !isError
    std::int64_t receivedAt = 0;                // epoch ms (disk LRU)
    std::string requestRevision;                // request fingerprint at send time (changed -> "stale response" badge)
    std::uint64_t bytes = 0;                    // estimated size for cap accounting (computed if = 0)
};

std::uint64_t estimateBytes(const ResponseRecord& r);

// Thread-safe internally (own mutex).
class ICacheDriver {
public:
    virtual ~ICacheDriver() = default;
    virtual std::optional<ResponseRecord> get(const std::string& id) = 0;
    // false = rejected (e.g. single record > cap -> not cached at this tier).
    virtual bool put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) = 0;
    // A driver that KEEPS a copy (e.g. RAM) can override to move; default delegates to the const& version.
    virtual bool put(const std::string& id, ResponseRecord&& r, std::uint64_t bytes) {
        return put(id, static_cast<const ResponseRecord&>(r), bytes);
    }
    virtual void remove(const std::string& id) = 0;
    virtual void clear() = 0;
    virtual void setCapBytes(std::uint64_t cap) = 0;   // change cap at runtime -> evict if needed
    // Persist deferred metadata (default no-op); called on shutdown because dtors don't run on macOS app terminate.
    virtual void flush() {}
    virtual std::uint64_t usedBytes() const = 0;
    virtual const char* name() const = 0;              // "ram" | "disk" | future…
};

// L1(RAM) -> L2(Disk): look up RAM first; write-through + prefer RAM when small.
class ResponseCache {
public:
    // l2 = nullptr if persist off (RAM only).
    ResponseCache(std::unique_ptr<ICacheDriver> l1,
                  std::unique_ptr<ICacheDriver> l2,
                  std::uint64_t ramThresholdBytes);
    ~ResponseCache();

    // sessionDir = the collection's .session dir (disk stores under sessionDir/responses/).
    static std::unique_ptr<ResponseCache> create(const CacheConfig& cfg, const std::string& sessionDir);

    std::optional<ResponseRecord> get(const std::string& id);   // RAM first -> disk -> promote
    void put(const std::string& id, ResponseRecord r);          // write-through + prefer RAM
    void remove(const std::string& id);                         // remove from both tiers
    void clear();
    void flush();                                               // persist deferred metadata on both tiers
    void onConfigChanged(const CacheConfig& c);                 // reload cap + threshold -> evict

    std::uint64_t l1UsedBytes() const;
    std::uint64_t l2UsedBytes() const;

private:
    std::unique_ptr<ICacheDriver> l1_;
    std::unique_ptr<ICacheDriver> l2_;   // may be null (persist off)
    std::uint64_t ramThresholdBytes_;
};

} // namespace core
