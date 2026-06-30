#include "infra/cache/disk_cache_driver.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "infra/cache/cache_codec.hpp"
#include "infra/fs_util.hpp"

namespace fs = std::filesystem;

namespace core {

namespace {
const char* kIndexFile = "_index.json";
}

std::string DiskCacheDriver::fileFor(const std::string& id) const {
    // The cache key is a request id, which the upper layer guarantees filesystem-safe (genId -> [a-z0-9];
    // isValidFileId enforces it). So the filename IS the id — no sanitizing, no hashing, no backup naming.
    return (fs::path(dir_) / (id + ".json")).string();
}

DiskCacheDriver::DiskCacheDriver(std::string dir, std::uint64_t capBytes)
    : dir_(std::move(dir)), cap_(capBytes) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    loadIndex();
}

DiskCacheDriver::~DiskCacheDriver() {
    std::lock_guard<std::mutex> lk(mu_);
    if (dirty_) persistIndex();     // flush latest atime (get only updates RAM — §1.1)
}

void DiskCacheDriver::loadIndex() {
    std::string txt;
    if (!fsutil::readFile((fs::path(dir_) / kIndexFile).string(), txt)) return;
    try {
        auto j = nlohmann::json::parse(txt);
        tick_ = j.value("tick", (std::int64_t)0);
        used_ = 0;
        // Bind to a named variable: .items() on a temporary -> dangling iterator (nlohmann pitfall).
        nlohmann::json entries = j.value("entries", nlohmann::json::object());
        for (auto& [id, e] : entries.items()) {
            IndexEntry ie;
            ie.bytes = e.value("bytes", (std::uint64_t)0);
            ie.receivedAt = e.value("receivedAt", (std::int64_t)0);
            ie.atime = e.value("atime", (std::int64_t)0);
            // L7: trust the index — don't stat every entry at startup (O(n) syscalls). A missing file
            // self-heals lazily in get() (readFile fails -> entry dropped), so a stale entry is harmless.
            used_ += ie.bytes;
            index_[id] = ie;
        }
        // Rebuild the LRU list by atime (newest at front) -> O(1) eviction later (§1.2).
        std::vector<std::pair<std::int64_t, std::string>> byAtime;
        byAtime.reserve(index_.size());
        for (const auto& [id, e] : index_) byAtime.emplace_back(e.atime, id);
        std::sort(byAtime.begin(), byAtime.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; }); // descending
        for (const auto& [atime, id] : byAtime) {
            lru_.push_back(id);
            index_[id].lru = std::prev(lru_.end());
        }
    } catch (...) { index_.clear(); lru_.clear(); used_ = 0; tick_ = 0; }
}

void DiskCacheDriver::persistIndex() const {
    nlohmann::json entries = nlohmann::json::object();
    for (const auto& [id, e] : index_)
        entries[id] = {{"bytes", e.bytes}, {"receivedAt", e.receivedAt}, {"atime", e.atime}};
    nlohmann::json j{{"tick", tick_}, {"entries", entries}};
    try { fsutil::writeFileAtomic((fs::path(dir_) / kIndexFile).string(), j.dump()); dirty_ = false; }
    catch (...) { /* index is a derived cache; write error -> ignore */ }
}

// Flush pending atime updates (get marks dirty_ but defers the write — §1.1). Called on shutdown
// (Engine::flushCache, since dtors don't run on app terminate) so LRU order survives restart. (Fix 2)
void DiskCacheDriver::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (dirty_) persistIndex();
}

void DiskCacheDriver::touch(IndexEntry& e, const std::string& id) {
    lru_.erase(e.lru);
    lru_.push_front(id);
    e.lru = lru_.begin();
}

void DiskCacheDriver::evictToFit() {
    while (used_ > cap_ && !lru_.empty()) {
        const std::string victim = lru_.back();     // back = least recently accessed (O(1))
        auto it = index_.find(victim);
        std::error_code ec;
        fs::remove(fileFor(victim), ec);
        if (it != index_.end()) { used_ -= it->second.bytes; index_.erase(it); }
        lru_.pop_back();
    }
}

std::optional<ResponseRecord> DiskCacheDriver::get(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    std::string txt;
    if (!fsutil::readFile(fileFor(id), txt)) {        // file gone -> clean up index
        used_ -= it->second.bytes;
        lru_.erase(it->second.lru);
        index_.erase(it);
        persistIndex();                               // file gone -> drop index entry (structural -> durable now)
        return std::nullopt;
    }
    try {
        ResponseRecord rec = cachecodec::fromJson(txt);
        it->second.atime = ++tick_;                   // last-access -> LRU (RAM only)
        touch(it->second, id);
        dirty_ = true;                                // §1.1: do NOT write index on every read; flush later
        return rec;
    } catch (...) { return std::nullopt; }
}

bool DiskCacheDriver::put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    if (bytes > cap_) {                               // body > cap -> skip cache (§5)
        auto old = index_.find(id);
        if (old != index_.end()) {
            std::error_code ec; fs::remove(fileFor(id), ec);
            used_ -= old->second.bytes;
            lru_.erase(old->second.lru);
            index_.erase(old);
            persistIndex();
        }
        return false;
    }
    try { fsutil::writeFileAtomic(fileFor(id), cachecodec::toJson(r)); }
    catch (...) { return false; }
    auto it = index_.find(id);
    if (it != index_.end()) {                         // overwrite: subtract old size + drop old LRU node
        used_ -= it->second.bytes;
        lru_.erase(it->second.lru);
        index_.erase(it);
    }
    lru_.push_front(id);
    IndexEntry ie;
    ie.bytes = bytes;
    ie.receivedAt = r.receivedAt;
    ie.atime = ++tick_;
    ie.lru = lru_.begin();
    used_ += bytes;
    index_[id] = ie;
    evictToFit();                                     // bound disk
    persistIndex();                                   // Fix 1: structural change -> persist NOW (response
                                                      // file already durable). Closes the data-loss window
                                                      // where a sparse stream's entry was never flushed.
    return true;
}

void DiskCacheDriver::remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(id);
    std::error_code ec;
    fs::remove(fileFor(id), ec);
    if (it != index_.end()) {
        used_ -= it->second.bytes;
        lru_.erase(it->second.lru);
        index_.erase(it);
        persistIndex();              // Fix 1: structural change -> durable now
    }
}

void DiskCacheDriver::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, e] : index_) { std::error_code ec; fs::remove(fileFor(id), ec); }
    index_.clear();
    lru_.clear();
    used_ = 0;
    persistIndex();                  // Fix 1: write the empty index now (tiny; don't rely on a dtor that may not run)
}

void DiskCacheDriver::setCapBytes(std::uint64_t cap) {
    std::lock_guard<std::mutex> lk(mu_);
    cap_ = cap;
    evictToFit();                    // eviction removes files immediately; persist the trimmed index now
    persistIndex();                  // Fix 1: keep on-disk index in sync with the eviction (rare op)
}

std::uint64_t DiskCacheDriver::usedBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return used_;
}

} // namespace core
