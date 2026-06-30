// cache_config.hpp — INTERNAL (core/src). The response-cache config builder, shared by the Engine
// (engine_impl.hpp) and the native CoreApiClient cache (composition_root.cpp). Extracted so the native
// path can build the exact same CacheConfig without depending on Engine::Impl — survives Engine deletion.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "core/infra/cache/cache.hpp"      // CacheConfig + CacheLimits
#include "core/domain/environment/env_config.hpp" // AppConfig

namespace core::detail {

// Read a numeric env var (>0) or fall back to default. ENV layer = hard ceiling (RESPONSE_CACHE §1).
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

// effective = clamp(user, min, max); user outside [min,max] -> clamp + warning log (RESPONSE_CACHE §1.2).
inline CacheConfig buildCacheConfig(const AppConfig& app, const CacheLimits& lim) {
    std::uint64_t ramMaxMb = limitOr(lim.ramMaxMb, "DEED_RAM_CACHE_SIZE_MAX", 256);
    std::uint64_t ramMinMb = limitOr(lim.ramMinMb, "DEED_RAM_CACHE_SIZE_MIN", 0);
    std::uint64_t diskMaxMb = limitOr(lim.diskMaxMb, "DEED_DISK_CACHE_SIZE_MAX", 1024);
    std::uint64_t diskMinMb = limitOr(lim.diskMinMb, "DEED_DISK_CACHE_SIZE_MIN", 0);
    std::uint64_t thrKb = limitOr(lim.thresholdKb, "DEED_RAM_CACHE_THRESHOLD_KB", 256);

    // clamp(user, min, max) + log when clamped. min > max (misconfig) -> prefer max as ceiling.
    auto clampMb = [](const char* what, std::uint64_t user, std::uint64_t mn, std::uint64_t mx) {
        if (mn > mx) mn = mx;
        std::uint64_t v = user;
        if (v > mx) { v = mx; std::fprintf(stderr, "[cache] %s=%lluMB > max %lluMB -> clamped to %lluMB\n",
                                           what, (unsigned long long)user, (unsigned long long)mx, (unsigned long long)mx); }
        if (v < mn) { std::fprintf(stderr, "[cache] %s=%lluMB < min %lluMB -> raised to %lluMB\n",
                                   what, (unsigned long long)v, (unsigned long long)mn, (unsigned long long)mn); v = mn; }
        return v;
    };

    // Read the LEVEL from the USER CONFIG (AppConfig) — this is the user layer (Settings).
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
