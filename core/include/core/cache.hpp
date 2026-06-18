// core/cache.hpp — Response cache hai tầng (RAM L1 -> Disk L2). RESPONSE_CACHE.md.
// Khoá theo request `id` ổn định; mỗi id chỉ giữ 1 response mới nhất. RAM bị chặn cứng
// theo CacheConfig (kẹp từ env+user, build ở Engine). Driver trừu tượng qua ICacheDriver
// -> thêm backend (sqlite/mmap…) chỉ cần implement interface, KHÔNG đụng facade/Engine.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "core/types.hpp"

namespace core {

// Cấu hình HIỆU LỰC (đã kẹp env/user) phơi cho cache. Code cache KHÔNG tự đọc env/AppConfig.
struct CacheConfig {
    std::uint64_t ramEffBytes = 64ull * 1024 * 1024;   // mức RAM hiệu lực
    std::uint64_t diskEffBytes = 256ull * 1024 * 1024; // mức disk hiệu lực
    std::uint64_t thresholdBytes = 256ull * 1024;      // response < ngưỡng -> ưu tiên RAM
    bool enabled = true;   // tắt -> không cache gì
    bool persist = true;   // tắt -> chỉ RAM (không gắn L2 disk)
};

// Bản ghi response mới nhất của 1 request.
struct ResponseRecord {
    bool isError = false;                      // true -> lưu lỗi (mạng/timeout/cancel) để hiện lại
    ErrorKind errorKind = ErrorKind::Unknown;  // hợp lệ khi isError
    std::string errorMessage;
    ApiResponse response;                      // hợp lệ khi !isError
    std::int64_t receivedAt = 0;               // epoch ms (LRU disk)
    std::string requestRevision;               // dấu vết request lúc gửi (đổi -> badge "response cũ")
    std::uint64_t bytes = 0;                   // size ước lượng để hạch toán cap (tính nếu = 0)
};

// Ước lượng tổng bytes của 1 record (body + headers + meta) để hạch toán cap.
std::uint64_t estimateBytes(const ResponseRecord& r);

// --- Driver trừu tượng (RESPONSE_CACHE.md §3). Thread-safe nội bộ (mutex riêng). ---
class ICacheDriver {
public:
    virtual ~ICacheDriver() = default;
    virtual std::optional<ResponseRecord> get(const std::string& id) = 0;
    // false = từ chối (vd record đơn lẻ > cap -> không cache tầng này).
    virtual bool put(const std::string& id, const ResponseRecord& r, std::uint64_t bytes) = 0;
    virtual void remove(const std::string& id) = 0;
    virtual void clear() = 0;
    virtual void setCapBytes(std::uint64_t cap) = 0;   // đổi cap runtime -> evict nếu cần
    virtual std::uint64_t usedBytes() const = 0;
    virtual const char* name() const = 0;              // "ram" | "disk" | tương lai…
};

// --- Facade L1(RAM) -> L2(Disk). Tìm RAM trước; ghi write-through + ưu tiên RAM khi nhỏ. ---
class ResponseCache {
public:
    // l2 = nullptr nếu persist tắt (chỉ RAM). Nhận sở hữu driver -> cho phép inject driver giả (test/§10).
    ResponseCache(std::unique_ptr<ICacheDriver> l1,
                  std::unique_ptr<ICacheDriver> l2,
                  std::uint64_t ramThresholdBytes);
    ~ResponseCache();

    // Dựng cache mặc định từ CacheConfig: RAM L1 + (persist ? Disk L2 : none).
    // sessionDir = thư mục .session của collection (disk lưu ở sessionDir/responses/).
    static std::unique_ptr<ResponseCache> create(const CacheConfig& cfg, const std::string& sessionDir);

    std::optional<ResponseRecord> get(const std::string& id);   // RAM trước -> disk -> promote
    void put(const std::string& id, ResponseRecord r);          // write-through + ưu tiên RAM
    void remove(const std::string& id);                         // xoá cả 2 tầng
    void clear();
    void onConfigChanged(const CacheConfig& c);                 // reload cap + threshold -> evict

    // Quan sát (test/diagnostic).
    std::uint64_t l1UsedBytes() const;
    std::uint64_t l2UsedBytes() const;

private:
    std::unique_ptr<ICacheDriver> l1_;
    std::unique_ptr<ICacheDriver> l2_;   // có thể null (persist off)
    std::uint64_t ramThresholdBytes_;
};

} // namespace core
