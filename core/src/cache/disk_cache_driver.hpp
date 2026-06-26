// disk_cache_driver.hpp — L2 on-disk cache: .session/responses/<id>.json + _index.json.
// Lightweight index to account cap + LRU WITHOUT statting every file. RESPONSE_CACHE.md §3/§5.
#pragma once

#include <list>
#include <map>
#include <mutex>
#include <string>

#include "core/cache.hpp"

namespace core {

class DiskCacheDriver : public ICacheDriver {
public:
    // dir = response storage dir (e.g. <root>/.session/responses). Created if absent.
    DiskCacheDriver(std::string dir, std::uint64_t capBytes);
    ~DiskCacheDriver() override;    // flush index if still dirty (atime updated on get)

    std::optional<ResponseRecord> get(const std::string& id) override;
    bool put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) override;
    void remove(const std::string& id) override;
    void clear() override;
    void setCapBytes(std::uint64_t cap) override;
    void flush() override;          // persist pending atime updates (Fix 2; dtors don't run on app terminate)
    std::uint64_t usedBytes() const override;
    const char* name() const override { return "disk"; }

private:
    struct IndexEntry {
        std::uint64_t bytes = 0;
        std::int64_t receivedAt = 0;
        std::int64_t atime = 0;     // last-access tick (LRU; persisted to restore order after restart)
        std::list<std::string>::iterator lru;  // position in lru_ (front = most recently used)
    };
    void loadIndex();               // read _index.json (called once at init); trusts the index, no FS scan
    void persistIndex() const;      // rewrite _index.json (atomic) + clear dirty_
    void touch(IndexEntry& e, const std::string& id);  // move id to LRU front (O(1))
    void evictToFit();              // pop back (oldest-atime) until used_ <= cap_ (O(1)/victim)
    std::string fileFor(const std::string& id) const;  // <dir>/<id>.json (id is guaranteed FS-safe upstream)

    mutable std::mutex mu_;
    std::string dir_;
    std::uint64_t cap_;
    std::uint64_t used_ = 0;
    std::int64_t tick_ = 0;         // incremented per access -> LRU order
    mutable bool dirty_ = false;    // atime changed on get but not yet flushed (§1.1; structural changes persist now)
    std::map<std::string, IndexEntry> index_;
    std::list<std::string> lru_;    // front = recently used, back = oldest (evict O(1) — §1.2)
};

} // namespace core
