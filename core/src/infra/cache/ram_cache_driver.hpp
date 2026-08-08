// L1 RAM cache, LRU by last-access; mutexed because put (background) and get (main) run concurrently.
#pragma once

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/infra/cache/cache.hpp"

namespace core {

class RamCacheDriver : public ICacheDriver {
public:
    explicit RamCacheDriver(std::uint64_t capBytes) : cap_(capBytes) {}

    std::optional<ResponseRecord> get(const std::string& id) override;
    bool put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) override;
    bool put(const std::string& id, ResponseRecord&& r, std::uint64_t bytes) override;
    void remove(const std::string& id) override;
    void clear() override;
    void setCapBytes(std::uint64_t cap) override;
    std::uint64_t usedBytes() const override;
    const char* name() const override { return "ram"; }

private:
    struct Entry {
        ResponseRecord rec;
        std::uint64_t bytes = 0;
        std::list<std::string>::iterator lru;  // position in lru_ (front = newest)
    };
    void touch(const std::string& id);
    void evictToFit();
    // Copy if lvalue, move if rvalue; defined in the .cpp.
    template <class R> bool putImpl(const std::string& id, R&& r, std::uint64_t bytes);

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> map_;
    std::list<std::string> lru_;                // front = recently used, back = oldest
    std::uint64_t cap_;
    std::uint64_t used_ = 0;
};

} // namespace core
