#include "cache/disk_cache_driver.hpp"

#include <cctype>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "codec/json_codec.hpp"
#include "infra/fs_util.hpp"

namespace fs = std::filesystem;

namespace core {

namespace {
const char* kIndexFile = "_index.json";
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
    } catch (...) { index_.clear(); used_ = 0; tick_ = 0; }
}

void DiskCacheDriver::persistIndex() const {
    nlohmann::json entries = nlohmann::json::object();
    for (const auto& [id, e] : index_)
        entries[id] = {{"bytes", e.bytes}, {"receivedAt", e.receivedAt}, {"atime", e.atime}};
    nlohmann::json j{{"tick", tick_}, {"entries", entries}};
    try { fsutil::writeFileAtomic((fs::path(dir_) / kIndexFile).string(), j.dump()); }
    catch (...) { /* index là cache dẫn xuất; lỗi ghi -> bỏ qua */ }
}

void DiskCacheDriver::evictToFit() {
    while (used_ > cap_ && !index_.empty()) {
        // chọn entry atime nhỏ nhất (lâu truy cập nhất).
        auto victim = index_.begin();
        for (auto it = index_.begin(); it != index_.end(); ++it)
            if (it->second.atime < victim->second.atime) victim = it;
        std::error_code ec;
        fs::remove(fileFor(victim->first), ec);
        used_ -= victim->second.bytes;
        index_.erase(victim);
    }
}

std::optional<ResponseRecord> DiskCacheDriver::get(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    std::string txt;
    if (!fsutil::readFile(fileFor(id), txt)) {        // file mất -> dọn index
        used_ -= it->second.bytes;
        index_.erase(it);
        persistIndex();
        return std::nullopt;
    }
    try {
        ResponseRecord rec = codec::responseRecordFromJson(nlohmann::json::parse(txt));
        it->second.atime = ++tick_;                   // last-access -> LRU
        persistIndex();
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
            index_.erase(old);
            persistIndex();
        }
        return false;
    }
    try { fsutil::writeFileAtomic(fileFor(id), codec::toJson(r).dump()); }
    catch (...) { return false; }
    auto it = index_.find(id);
    if (it != index_.end()) used_ -= it->second.bytes;   // ghi đè: trừ size cũ
    IndexEntry ie;
    ie.bytes = bytes;
    ie.receivedAt = r.receivedAt;
    ie.atime = ++tick_;
    used_ += bytes;
    index_[id] = ie;
    evictToFit();                                     // bound disk
    persistIndex();
    return true;
}

void DiskCacheDriver::remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(id);
    std::error_code ec;
    fs::remove(fileFor(id), ec);
    if (it != index_.end()) { used_ -= it->second.bytes; index_.erase(it); persistIndex(); }
}

void DiskCacheDriver::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, e] : index_) { std::error_code ec; fs::remove(fileFor(id), ec); }
    index_.clear();
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
