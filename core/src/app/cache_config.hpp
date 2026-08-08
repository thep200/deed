#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "core/infra/cache/cache.hpp"
#include "core/domain/environment/env_config.hpp"

namespace core::detail {

// Fallbacks used only when neither .env (CacheLimits) nor the DEED_* process env provides a value.
inline constexpr std::uint64_t kRamCacheMaxMbDefault = 256;
inline constexpr std::uint64_t kRamCacheMinMbDefault = 0;
inline constexpr std::uint64_t kDiskCacheMaxMbDefault = 1024;
inline constexpr std::uint64_t kDiskCacheMinMbDefault = 0;
inline constexpr std::uint64_t kRamCacheThresholdKbDefault = 256;

// Numeric env var (>0) or default; the env layer is the hard ceiling.
inline std::uint64_t envU64(const char* key, std::uint64_t def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
    try { long long n = std::stoll(v); return n > 0 ? static_cast<std::uint64_t>(n) : def; }
    catch (...) { return def; }
}

// Take ceiling/floor from CacheLimits (.env loaded by UI) if set; otherwise fall back to getenv then default.
inline std::uint64_t limitOr(int fromEnvFile, const char* envKey, std::uint64_t def) {
    if (fromEnvFile > 0) return static_cast<std::uint64_t>(fromEnvFile);
    return envU64(envKey, def);
}

// effective = clamp(user, min, max); out-of-range values are clamped with a warning log.
inline CacheConfig buildCacheConfig(const AppConfig& app, const CacheLimits& lim) {
    std::uint64_t ramMaxMb = limitOr(lim.ramMaxMb, "DEED_RAM_CACHE_SIZE_MAX", kRamCacheMaxMbDefault);
    std::uint64_t ramMinMb = limitOr(lim.ramMinMb, "DEED_RAM_CACHE_SIZE_MIN", kRamCacheMinMbDefault);
    std::uint64_t diskMaxMb = limitOr(lim.diskMaxMb, "DEED_DISK_CACHE_SIZE_MAX", kDiskCacheMaxMbDefault);
    std::uint64_t diskMinMb = limitOr(lim.diskMinMb, "DEED_DISK_CACHE_SIZE_MIN", kDiskCacheMinMbDefault);
    std::uint64_t thrKb = limitOr(lim.thresholdKb, "DEED_RAM_CACHE_THRESHOLD_KB", kRamCacheThresholdKbDefault);

    // min > max (misconfig) -> prefer max as ceiling.
    auto clampMb = [](const char* what, std::uint64_t user, std::uint64_t mn, std::uint64_t mx) {
        if (mn > mx) mn = mx;
        std::uint64_t v = user;
        if (v > mx) { v = mx; std::fprintf(stderr, "[cache] %s=%lluMB > max %lluMB -> clamped to %lluMB\n",
                                           what, (unsigned long long)user, (unsigned long long)mx, (unsigned long long)mx); }
        if (v < mn) { std::fprintf(stderr, "[cache] %s=%lluMB < min %lluMB -> raised to %lluMB\n",
                                   what, (unsigned long long)v, (unsigned long long)mn, (unsigned long long)mn); v = mn; }
        return v;
    };

    std::uint64_t ramUserMb = app.ramCacheSizeMb > 0 ? static_cast<std::uint64_t>(app.ramCacheSizeMb) : 0;
    std::uint64_t diskUserMb = app.diskCacheSizeMb > 0 ? static_cast<std::uint64_t>(app.diskCacheSizeMb) : 0;

    CacheConfig c;
    c.ramEffBytes = clampMb("ram_cache_size", ramUserMb, ramMinMb, ramMaxMb) * 1024ull * 1024ull;
    c.diskEffBytes = clampMb("disk_cache_size", diskUserMb, diskMinMb, diskMaxMb) * 1024ull * 1024ull;
    c.thresholdBytes = thrKb * 1024ull;
    c.enabled = app.cacheResponses;   // default true (not exposed in Settings)
    c.persist = app.cachePersist;     // default true
    return c;
}

} // namespace core::detail
