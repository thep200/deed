#include "cache/disk_cache_driver.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "codec/json_codec.hpp"
#include "infra/fs_util.hpp"

namespace fs = std::filesystem;

namespace core {

namespace {
const char* kIndexFile = "_index.json";
// Gộp ghi index: chỉ flush _index.json sau mỗi N thay đổi cấu trúc (put/remove) thay vì mỗi lần.
// File response đã ghi bền ngay (durable); index chỉ là metadata dẫn xuất — entry thiếu sau crash
// thành file mồ côi (cache miss, dọn ở clear), entry thừa tự lành ở loadIndex (kiểm fs::exists).
const int kIndexFlushEvery = 8;
}

std::string DiskCacheDriver::safeId(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '_' || c == '-') out += static_cast<char>(c);
        else out += '_';
    }
    if (out.empty()) out = "_";
    return out;
}

std::string DiskCacheDriver::fileFor(const std::string& id) const {
    return (fs::path(dir_) / (safeId(id) + ".json")).string();
}

DiskCacheDriver::DiskCacheDriver(std::string dir, std::uint64_t capBytes)
    : dir_(std::move(dir)), cap_(capBytes) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    loadIndex();
}

DiskCacheDriver::~DiskCacheDriver() {
    std::lock_guard<std::mutex> lk(mu_);
    if (dirty_) persistIndex();     // flush atime mới nhất (get chỉ cập nhật RAM — §1.1)
}

void DiskCacheDriver::loadIndex() {
    std::string txt;
    if (!fsutil::readFile((fs::path(dir_) / kIndexFile).string(), txt)) return;
    try {
        auto j = nlohmann::json::parse(txt);
        tick_ = j.value("tick", (std::int64_t)0);
        used_ = 0;
        // Bind vào biến có tên: .items() trên temporary -> iterator treo (pitfall nlohmann).
        nlohmann::json entries = j.value("entries", nlohmann::json::object());
        for (auto& [id, e] : entries.items()) {
            IndexEntry ie;
            ie.bytes = e.value("bytes", (std::uint64_t)0);
            ie.receivedAt = e.value("receivedAt", (std::int64_t)0);
            ie.atime = e.value("atime", (std::int64_t)0);
            // Bỏ qua entry mà file đã biến mất (đồng bộ với đĩa).
            if (!fs::exists(fileFor(id))) continue;
            used_ += ie.bytes;
            index_[id] = ie;
        }
        // Dựng lại LRU list theo atime (mới nhất ở front) -> evict O(1) sau này (§1.2).
        std::vector<std::pair<std::int64_t, std::string>> byAtime;
        byAtime.reserve(index_.size());
        for (const auto& [id, e] : index_) byAtime.emplace_back(e.atime, id);
        std::sort(byAtime.begin(), byAtime.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; }); // giảm dần
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
    try { fsutil::writeFileAtomic((fs::path(dir_) / kIndexFile).string(), j.dump()); dirty_ = false; unflushed_ = 0; }
    catch (...) { /* index là cache dẫn xuất; lỗi ghi -> bỏ qua */ }
}

// put/remove gọi đây thay vì persistIndex() trực tiếp: gộp tối đa kIndexFlushEvery thay đổi
// vào 1 lần ghi đĩa. Destructor flush phần dư khi dirty_ còn set.
void DiskCacheDriver::noteIndexDirty() {
    dirty_ = true;
    if (++unflushed_ >= kIndexFlushEvery) persistIndex();
}

void DiskCacheDriver::touch(IndexEntry& e, const std::string& id) {
    lru_.erase(e.lru);
    lru_.push_front(id);
    e.lru = lru_.begin();
}

void DiskCacheDriver::evictToFit() {
    while (used_ > cap_ && !lru_.empty()) {
        const std::string victim = lru_.back();     // back = lâu truy cập nhất (O(1))
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
    if (!fsutil::readFile(fileFor(id), txt)) {        // file mất -> dọn index
        used_ -= it->second.bytes;
        lru_.erase(it->second.lru);
        index_.erase(it);
        noteIndexDirty();                             // file mất -> gỡ index (gộp ghi)
        return std::nullopt;
    }
    try {
        ResponseRecord rec = codec::responseRecordFromJson(nlohmann::json::parse(txt));
        it->second.atime = ++tick_;                   // last-access -> LRU (chỉ RAM)
        touch(it->second, id);
        dirty_ = true;                                // §1.1: KHÔNG ghi index mỗi lần đọc; flush sau
        return rec;
    } catch (...) { return std::nullopt; }
}

bool DiskCacheDriver::put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    if (bytes > cap_) {                               // body > cap -> bỏ cache (§5)
        auto old = index_.find(id);
        if (old != index_.end()) {
            std::error_code ec; fs::remove(fileFor(id), ec);
            used_ -= old->second.bytes;
            lru_.erase(old->second.lru);
            index_.erase(old);
            noteIndexDirty();
        }
        return false;
    }
    try { fsutil::writeFileAtomic(fileFor(id), codec::toJson(r).dump()); }
    catch (...) { return false; }
    auto it = index_.find(id);
    if (it != index_.end()) {                         // ghi đè: trừ size cũ + gỡ node LRU cũ
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
    noteIndexDirty();                                 // gộp ghi index (file response đã bền)
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
        noteIndexDirty();
    }
}

void DiskCacheDriver::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, e] : index_) { std::error_code ec; fs::remove(fileFor(id), ec); }
    index_.clear();
    lru_.clear();
    used_ = 0;
    persistIndex();
}

void DiskCacheDriver::setCapBytes(std::uint64_t cap) {
    std::lock_guard<std::mutex> lk(mu_);
    cap_ = cap;
    evictToFit();
    persistIndex();
}

std::uint64_t DiskCacheDriver::usedBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return used_;
}

} // namespace core
