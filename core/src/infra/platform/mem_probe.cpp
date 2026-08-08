#include "core/infra/platform/mem_probe.hpp"

#include <chrono>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace core::memprobe {

std::uint64_t PhysFootprintBytes() {
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS)
        return info.phys_footprint;   // ~ "Memory" in Activity Monitor
#endif
    return 0;
}

static long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

StructuredLogger::StructuredLogger(const std::string& path) {
    out_.open(path, std::ios::out | std::ios::trunc);
    if (out_.is_open())
        out_ << "ts_ms,iter,op,phys_footprint_mb,ram_cache_bytes,disk_cache_bytes,open_request_id,idle\n";
}

StructuredLogger::~StructuredLogger() {
    if (out_.is_open()) out_.flush();
}

void StructuredLogger::log(const Row& r) {
    std::lock_guard<std::mutex> lk(m_);
    if (!out_.is_open()) return;
    double mb = static_cast<double>(PhysFootprintBytes()) / (1024.0 * 1024.0);
    out_ << NowMs() << ',' << r.iter << ',' << r.op << ','
         << mb << ',' << r.ramCacheBytes << ',' << r.diskCacheBytes << ','
         << r.openRequestId << ',' << (r.idle ? 1 : 0) << '\n';
}

void StructuredLogger::flush() {
    std::lock_guard<std::mutex> lk(m_);
    if (out_.is_open()) out_.flush();
}

} // namespace core::memprobe
