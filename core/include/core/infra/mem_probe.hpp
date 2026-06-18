// core/infra/mem_probe.hpp — đo RAM tiến trình + logger có cấu trúc cho stress harness.
// STRESS_TEST.md §3. Header public (cả CLI lẫn UI macOS link core đều dùng được).
#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace core::memprobe {

// phys_footprint (~ cột "Memory" trong Activity Monitor), tính bằng byte.
// Trả 0 nếu không lấy được (hoặc nền non-Apple — guard mach).
std::uint64_t PhysFootprintBytes();

// Logger CSV có cấu trúc, thread-safe. Mỗi iteration ghi 1 dòng (STRESS_TEST §3.2).
// Cột: ts_ms, iter, op, phys_footprint_mb, ram_cache_bytes, disk_cache_bytes, open_request_id, idle
// `idle=1` đánh dấu idle checkpoint (đã về trạng thái không mở request) -> phân tích baseline.
class StructuredLogger {
public:
    struct Row {
        long long iter = 0;
        std::string op;
        std::uint64_t ramCacheBytes = 0;
        std::uint64_t diskCacheBytes = 0;
        std::string openRequestId;
        bool idle = false;
    };

    explicit StructuredLogger(const std::string& path); // mở (truncate) + ghi header CSV
    ~StructuredLogger();

    StructuredLogger(const StructuredLogger&) = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;

    void log(const Row& r);   // tự lấy phys_footprint + timestamp khi ghi
    void flush();
    bool ok() const { return out_.is_open(); }

private:
    std::ofstream out_;
    std::mutex m_;
};

} // namespace core::memprobe
