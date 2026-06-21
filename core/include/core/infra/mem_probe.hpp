// core/infra/mem_probe.hpp — measure process RAM + structured logger for the stress harness.
// STRESS_TEST.md §3. Public header (usable by both CLI and the macOS UI that links core).
#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace core::memprobe {

// phys_footprint (~ "Memory" column in Activity Monitor), in bytes.
// Returns 0 if unavailable (or non-Apple platform — mach guard).
std::uint64_t PhysFootprintBytes();

// Thread-safe structured CSV logger. Each iteration writes one row (STRESS_TEST §3.2).
// Columns: ts_ms, iter, op, phys_footprint_mb, ram_cache_bytes, disk_cache_bytes, open_request_id, idle
// `idle=1` marks an idle checkpoint (back to no-open-request state) -> baseline analysis.
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

    explicit StructuredLogger(const std::string& path); // open (truncate) + write CSV header
    ~StructuredLogger();

    StructuredLogger(const StructuredLogger&) = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;

    void log(const Row& r);   // auto-captures phys_footprint + timestamp on write
    void flush();
    bool ok() const { return out_.is_open(); }

private:
    std::ofstream out_;
    std::mutex m_;
};

} // namespace core::memprobe
