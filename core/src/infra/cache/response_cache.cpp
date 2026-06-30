#include "core/infra/cache/cache.hpp"

#include <utility>   // std::move

#include "infra/cache/disk_cache_driver.hpp"
#include "infra/cache/ram_cache_driver.hpp"
#include "infra/fs_util.hpp"

namespace core {

std::uint64_t estimateBytes(const ResponseRecord& r) {
    if (r.bytes) return r.bytes;
    std::uint64_t b = r.errorMessage.size() + r.requestRevision.size() + 64; // meta
    b += r.response.body.size();
    for (const auto& h : r.response.headers) b += h.name.size() + h.value.size() + 8;
    for (const auto& c : r.response.cookies)
        b += c.name.size() + c.value.size() + c.domain.size() + c.path.size() + c.expires.size() + 8;
    return b;
}

ResponseCache::ResponseCache(std::unique_ptr<ICacheDriver> l1,
                             std::unique_ptr<ICacheDriver> l2,
                             std::uint64_t ramThresholdBytes)
    : l1_(std::move(l1)), l2_(std::move(l2)), ramThresholdBytes_(ramThresholdBytes) {}

ResponseCache::~ResponseCache() = default;

std::unique_ptr<ResponseCache> ResponseCache::create(const CacheConfig& cfg,
                                                     const std::string& sessionDir) {
    auto l1 = std::make_unique<RamCacheDriver>(cfg.ramEffBytes);
    std::unique_ptr<ICacheDriver> l2;
    if (cfg.persist) {
        std::string respDir = fsutil::join(sessionDir, "responses");
        l2 = std::make_unique<DiskCacheDriver>(respDir, cfg.diskEffBytes);
    }
    return std::make_unique<ResponseCache>(std::move(l1), std::move(l2), cfg.thresholdBytes);
}

std::optional<ResponseRecord> ResponseCache::get(const std::string& id) {
    if (id.empty()) return std::nullopt;
    if (auto r = l1_->get(id)) return r;             // L1 hit (RAM first)
    if (l2_) {
        if (auto r = l2_->get(id)) {                 // L2 hit
            std::uint64_t b = r->bytes ? r->bytes : estimateBytes(*r);
            // M7: promotion to L1 isn't atomic with a concurrent remove() on L2. A remove between this read
            // and the put could leave the entry in L1 after L2 dropped it — transient L1/L2 divergence that
            // self-heals (a later get re-reads L2, a later remove clears both). Accepted for a response cache.
            if (b < ramThresholdBytes_) l1_->put(id, *r, b);  // promote to RAM if small
            return r;
        }
    }
    return std::nullopt;                             // miss
}

void ResponseCache::put(const std::string& id, ResponseRecord r) {
    if (id.empty()) return;
    if (r.bytes == 0) r.bytes = estimateBytes(r);
    std::uint64_t b = r.bytes;
    if (l2_) l2_->put(id, r, b);                          // write-through (read r, serialize -> disk)
    if (b < ramThresholdBytes_) l1_->put(id, std::move(r), b);  // prefer RAM; move (last use of r)
    // b >= threshold -> disk only (does not occupy RAM)
}

void ResponseCache::remove(const std::string& id) {
    l1_->remove(id);
    if (l2_) l2_->remove(id);
}

void ResponseCache::clear() {
    l1_->clear();
    if (l2_) l2_->clear();
}

void ResponseCache::flush() {
    l1_->flush();
    if (l2_) l2_->flush();
}

void ResponseCache::onConfigChanged(const CacheConfig& c) {
    l1_->setCapBytes(c.ramEffBytes);
    if (l2_) l2_->setCapBytes(c.diskEffBytes);
    ramThresholdBytes_ = c.thresholdBytes;
}

std::uint64_t ResponseCache::l1UsedBytes() const { return l1_->usedBytes(); }
std::uint64_t ResponseCache::l2UsedBytes() const { return l2_ ? l2_->usedBytes() : 0; }

} // namespace core
