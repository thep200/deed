#include "cache/ram_cache_driver.hpp"

#include <utility>   // std::move, std::forward

namespace core {

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

// Shared body: R&& is a forwarding ref -> `e.rec = std::forward<R>(r)` copies if lvalue (const&),
// moves if rvalue. Defined here (same TU as the two overloads below) so no explicit instantiation needed.
template <class R>
bool RamCacheDriver::putImpl(const std::string& id, R&& r, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    // Single record larger than cap -> don't cache in RAM (caller uses disk). §5.
    if (bytes > cap_) {
        // if an old entry with the same id is held -> drop it to keep accounting consistent.
        auto old = map_.find(id);
        if (old != map_.end()) { used_ -= old->second.bytes; lru_.erase(old->second.lru); map_.erase(old); }
        return false;
    }
    auto it = map_.find(id);
    if (it != map_.end()) {          // overwrite: subtract old size first
        used_ -= it->second.bytes;
        lru_.erase(it->second.lru);
        map_.erase(it);
    }
    lru_.push_front(id);
    Entry e;
    e.rec = std::forward<R>(r);      // move if caller passes an rvalue -> avoid copying a large body
    e.bytes = bytes;
    e.lru = lru_.begin();
    used_ += bytes;
    map_.emplace(id, std::move(e));
    evictToFit();                    // bound: evict LRU until within cap
    return true;
}

bool RamCacheDriver::put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) {
    return putImpl(id, r, bytes);                 // lvalue -> copy
}
bool RamCacheDriver::put(const std::string& id, ResponseRecord&& r, std::uint64_t bytes) {
    return putImpl(id, std::move(r), bytes);       // rvalue -> move
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
    evictToFit();                    // smaller cap -> evict immediately
}

std::uint64_t RamCacheDriver::usedBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return used_;
}

} // namespace core
