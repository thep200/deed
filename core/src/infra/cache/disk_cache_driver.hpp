// L2 on-disk cache; the index accounts cap + LRU without statting every file.
#pragma once

#include <list>
#include <map>
#include <mutex>
#include <string>

#include "core/infra/cache/cache.hpp"

namespace core {

class DiskCacheDriver : public ICacheDriver {
public:
    // dir is created if absent.
    DiskCacheDriver(std::string dir, std::uint64_t capBytes);
    ~DiskCacheDriver() override;    // flush index if still dirty (atime updated on get)

    std::optional<ResponseRecord> get(const std::string& id) override;
    bool put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) override;
    void remove(const std::string& id) override;
    void clear() override;
    void setCapBytes(std::uint64_t cap) override;
    void flush() override;          // persist pending atime updates (dtors may not run on app terminate)
    std::uint64_t usedBytes() const override;
    const char* name() const override { return "disk"; }

private:
    struct IndexEntry {
        std::uint64_t bytes = 0;
        std::int64_t receivedAt = 0;
        std::int64_t atime = 0;     // last-access tick (LRU; persisted to restore order after restart)
        std::list<std::string>::iterator lru;  // position in lru_ (front = most recently used)
    };
    void loadIndex();               // trusts the index, no FS scan
    void persistIndex() const;      // atomic rewrite + clear dirty_
    void touch(IndexEntry& e, const std::string& id);
    void evictToFit();
    std::string fileFor(const std::string& id) const;

    mutable std::mutex mu_;
    std::string dir_;
    std::uint64_t cap_;
    std::uint64_t used_ = 0;
    std::int64_t tick_ = 0;         // incremented per access -> LRU order
    mutable bool dirty_ = false;    // atime changed on get but not yet flushed (structural changes persist immediately)
    std::map<std::string, IndexEntry> index_;
    std::list<std::string> lru_;    // front = recently used, back = oldest
};

} // namespace core
