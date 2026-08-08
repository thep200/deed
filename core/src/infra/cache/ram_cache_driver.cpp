#include "infra/cache/ram_cache_driver.hpp"

#include <utility>

namespace core {

namespace {
// Charge fixed overhead + the duplicated key so used_ tracks the real footprint (map/LRU nodes, id
// stored twice) — many tiny entries can't blow the cap, and overhead also bounds entry count.
constexpr std::uint64_t kEntryOverheadBytes = 256;
std::uint64_t effBytesFor(const std::string& id, std::uint64_t payload) {
    return payload + kEntryOverheadBytes + 2 * static_cast<std::uint64_t>(id.size());
}
} // namespace

void RamCacheDriver::touch(const std::string& id) {
    auto it = map_.find(id);
    if (it == map_.end()) return;
    lru_.erase(it->second.lru);
    lru_.push_front(id);
    it->second.lru = lru_.begin();
}

void RamCacheDriver::evictToFit() {
    while (used_ > cap_ && !lru_.empty()) {
        const std::string& victim = lru_.back();
        auto it = map_.find(victim);
        if (it != map_.end()) {
            used_ -= it->second.bytes;
            map_.erase(it);
        }
        lru_.pop_back();
    }
}

std::optional<ResponseRecord> RamCacheDriver::get(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(id);
    if (it == map_.end()) return std::nullopt;
    touch(id);                       // last-access -> LRU front
    return it->second.rec;
}

// Forwarding ref: copy if lvalue, move if rvalue; defined in this TU so no explicit instantiation is needed.
template <class R>
bool RamCacheDriver::putImpl(const std::string& id, R&& r, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::uint64_t eff = effBytesFor(id, bytes);
    // Record larger than cap -> not cached in RAM (caller uses disk).
    if (eff > cap_) {
        // an old entry with the same id must still be dropped to keep accounting consistent.
        auto old = map_.find(id);
        if (old != map_.end()) { used_ -= old->second.bytes; lru_.erase(old->second.lru); map_.erase(old); }
        return false;
    }
    auto it = map_.find(id);
    if (it != map_.end()) {
        used_ -= it->second.bytes;
        lru_.erase(it->second.lru);
        map_.erase(it);
    }
    lru_.push_front(id);
    Entry e;
    e.rec = std::forward<R>(r);
    e.bytes = eff;                   // charge the effective footprint, not just the payload
    e.lru = lru_.begin();
    used_ += eff;
    map_.emplace(id, std::move(e));
    evictToFit();
    return true;
}

bool RamCacheDriver::put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) {
    return putImpl(id, r, bytes);
}
bool RamCacheDriver::put(const std::string& id, ResponseRecord&& r, std::uint64_t bytes) {
    return putImpl(id, std::move(r), bytes);
}

void RamCacheDriver::remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(id);
    if (it == map_.end()) return;
    used_ -= it->second.bytes;
    lru_.erase(it->second.lru);
    map_.erase(it);
}

void RamCacheDriver::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear();
    lru_.clear();
    used_ = 0;
}

void RamCacheDriver::setCapBytes(std::uint64_t cap) {
    std::lock_guard<std::mutex> lk(mu_);
    cap_ = cap;
    evictToFit();
}

std::uint64_t RamCacheDriver::usedBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return used_;
}

} // namespace core
