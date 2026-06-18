// ram_cache_driver.hpp — L1 cache trong RAM. LRU theo last-access, bound theo cap.
// RESPONSE_CACHE.md §3/§5. Thread-safe (mutex) vì put (nền) và get (main) chạy song song.
#pragma once

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/cache.hpp"

namespace core {

class RamCacheDriver : public ICacheDriver {
public:
    explicit RamCacheDriver(std::uint64_t capBytes) : cap_(capBytes) {}

    std::optional<ResponseRecord> get(const std::string& id) override;
    bool put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) override;
    void remove(const std::string& id) override;
    void clear() override;
    void setCapBytes(std::uint64_t cap) override;
    std::uint64_t usedBytes() const override;
    const char* name() const override { return "ram"; }

private:
    struct Entry {
        ResponseRecord rec;
        std::uint64_t bytes = 0;
        std::list<std::string>::iterator lru;  // vị trí trong lru_ (front = mới nhất)
    };
    void touch(const std::string& id);          // đưa id lên front LRU
    void evictToFit();                          // evict back (cũ nhất) tới khi used_ <= cap_

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> map_;
    std::list<std::string> lru_;                // front = vừa dùng, back = lâu nhất
    std::uint64_t cap_;
    std::uint64_t used_ = 0;
};

} // namespace core
