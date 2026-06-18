// disk_cache_driver.hpp — L2 cache trên đĩa: .session/responses/<id>.json + _index.json.
// Index nhẹ để hạch toán cap + LRU mà KHÔNG stat toàn bộ file. RESPONSE_CACHE.md §3/§5.
#pragma once

#include <list>
#include <map>
#include <mutex>
#include <string>

#include "core/cache.hpp"

namespace core {

class DiskCacheDriver : public ICacheDriver {
public:
    // dir = thư mục lưu response (vd <root>/.session/responses). Tạo nếu chưa có.
    DiskCacheDriver(std::string dir, std::uint64_t capBytes);
    ~DiskCacheDriver() override;    // flush index nếu còn dirty (atime cập nhật khi get)

    std::optional<ResponseRecord> get(const std::string& id) override;
    bool put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) override;
    void remove(const std::string& id) override;
    void clear() override;
    void setCapBytes(std::uint64_t cap) override;
    std::uint64_t usedBytes() const override;
    const char* name() const override { return "disk"; }

private:
    struct IndexEntry {
        std::uint64_t bytes = 0;
        std::int64_t receivedAt = 0;
        std::int64_t atime = 0;     // last-access tick (LRU; persist để khôi phục thứ tự sau restart)
        std::list<std::string>::iterator lru;  // vị trí trong lru_ (front = mới dùng nhất)
    };
    void loadIndex();               // đọc _index.json (gọi 1 lần lúc khởi tạo)
    void persistIndex() const;      // ghi lại _index.json (atomic) + clear dirty_
    void touch(IndexEntry& e, const std::string& id);  // đưa id lên front LRU (O(1))
    void evictToFit();              // pop back (oldest-atime) tới khi used_ <= cap_ (O(1)/victim)
    std::string fileFor(const std::string& id) const;  // <dir>/<safeId>.json
    static std::string safeId(const std::string& id);

    mutable std::mutex mu_;
    std::string dir_;
    std::uint64_t cap_;
    std::uint64_t used_ = 0;
    std::int64_t tick_ = 0;         // tăng mỗi truy cập -> thứ tự LRU
    mutable bool dirty_ = false;    // atime đổi khi get (chỉ RAM) -> cần flush; tránh ghi mỗi đọc (§1.1)
    std::map<std::string, IndexEntry> index_;
    std::list<std::string> lru_;    // front = vừa dùng, back = lâu nhất (evict O(1) — §1.2)
};

} // namespace core
