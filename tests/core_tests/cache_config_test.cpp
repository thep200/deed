#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/infra/cache/cache.hpp"
#include "core/infra/persistence/stores.hpp"
#include "app/cache_config.hpp" // detail::buildCacheConfig (core/src; white-box include path)

namespace fs = std::filesystem;
using namespace core;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) { ++g_pass; }                                            \
        else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                \
    do {                                                                   \
        auto _va = (a); auto _vb = (b);                                    \
        if (_va == _vb) { ++g_pass; }                                      \
        else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

static std::string makeTempRoot() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = fs::temp_directory_path() / ("apiclient_test_" + std::to_string(stamp));
    fs::remove_all(base);
    fs::create_directories(base);
    return base.string();
}

static ResponseRecord mkRec(size_t bodyLen, int status) {
    ResponseRecord r;
    r.isError = false;
    r.response.statusCode = status;
    r.response.body = std::string(bodyLen, 'x');
    r.receivedAt = 1;
    return r;
}

static void test_response_cache(const std::string& root) {
    std::printf("[response_cache]\n");
    CacheConfig cfg;
    cfg.ramEffBytes = 4 * 1024;      // small RAM cap to test LRU
    cfg.diskEffBytes = 1024 * 1024;
    cfg.thresholdBytes = 1024;       // < 1KB -> prefer RAM
    cfg.enabled = true;
    cfg.persist = true;
    std::string sdir = (fs::path(root) / ".session_cache_test").string();
    fs::remove_all(sdir);
    auto cache = ResponseCache::create(cfg, sdir);

    cache->put("a", mkRec(100, 200));
    CHECK(cache->l1UsedBytes() > 0, "small response goes to RAM");
    CHECK(cache->l2UsedBytes() > 0, "small response write-through disk");
    auto ga = cache->get("a");
    CHECK(ga && ga->response.statusCode == 200, "get a hits L1");

    std::uint64_t l1Before = cache->l1UsedBytes();
    cache->put("big", mkRec(4000, 200));
    CHECK_EQ(cache->l1UsedBytes(), l1Before, "large response NOT in RAM");
    auto gb = cache->get("big");
    CHECK(gb && gb->response.body.size() == 4000, "get big hits (from disk)");

    for (int i = 0; i < 50; i++) cache->put("k" + std::to_string(i), mkRec(200, 200));
    CHECK(cache->l1UsedBytes() <= cfg.ramEffBytes, "RAM cache does not exceed cap (LRU evict)");

    cache->remove("big");
    CHECK(!cache->get("big"), "remove -> miss");

    cache->put("small2", mkRec(50, 201));
    cache.reset();
    auto cache2 = ResponseCache::create(cfg, sdir);
    CHECK_EQ(cache2->l1UsedBytes(), (std::uint64_t)0, "restart: L1 empty");
    auto gs = cache2->get("small2");
    CHECK(gs && gs->response.statusCode == 201, "restart: read small2 from disk");
    CHECK(cache2->l1UsedBytes() > 0, "small disk-hit -> promote to RAM");

    struct NullDriver : ICacheDriver {
        std::optional<ResponseRecord> get(const std::string&) override { return std::nullopt; }
        bool put(const std::string&, const ResponseRecord&, std::uint64_t) override { return false; }
        void remove(const std::string&) override {}
        void clear() override {}
        void setCapBytes(std::uint64_t) override {}
        std::uint64_t usedBytes() const override { return 0; }
        const char* name() const override { return "null"; }
    };
    ResponseCache nullCache(std::make_unique<NullDriver>(), std::make_unique<NullDriver>(), 1024);
    nullCache.put("x", mkRec(10, 200));
    CHECK(!nullCache.get("x"), "NullCacheDriver plugs into facade (without touching ResponseCache)");

    fs::remove_all(sdir);
}

static void test_cache_durability(const std::string& root) {
    std::printf("[cache_durability]\n");
    CacheConfig cfg;
    cfg.ramEffBytes = 1024 * 1024;
    cfg.diskEffBytes = 1024 * 1024;
    cfg.thresholdBytes = 1024;
    cfg.enabled = true; cfg.persist = true;

    std::string sdir = (fs::path(root) / ".session_durability").string();
    fs::remove_all(sdir);
    {
        auto c1 = ResponseCache::create(cfg, sdir);
        c1->put("strm_a16z", mkRec(500, 200));         // single sparse put, like a stream close
        auto c2 = ResponseCache::create(cfg, sdir);    // NO dtor on c1 -> only succeeds if put wrote the index
        auto got = c2->get("strm_a16z");
        CHECK(got.has_value() && got->response.statusCode == 200,
              "Fix 1: put persisted to index immediately (survives w/o dtor)");
    }

    // Filename = id.json directly (no sanitize, no hash) — this layer trusts the upstream-safe id.
    std::string sdir2 = (fs::path(root) / ".session_filename").string();
    fs::remove_all(sdir2);
    fs::path respDir = fs::path(sdir2) / "responses";
    {
        auto c = ResponseCache::create(cfg, sdir2);
        c->put("cleanid123", mkRec(300, 200));
        CHECK(fs::exists(respDir / "cleanid123.json"), "filename is exactly <id>.json (no hash/sanitize)");
        CHECK(c->get("cleanid123").has_value(), "round-trips by bare id");
    }
    fs::remove_all(sdir); fs::remove_all(sdir2);
}

static void test_cache_config_clamp(const std::string& root) {
    std::printf("[cache_config]\n");

    // (1) Via getenv (ops): user > max -> clamp to max.
    {
        std::string cfgPath = (fs::path(root) / "appcfg_cache.json").string();
        AppConfig ac;
        ac.ramCacheSizeMb = 256;     // user > env max -> must clamp
        ac.diskCacheSizeMb = 2048;
        AppConfigStore(cfgPath).save(ac);
        AppConfig reread = AppConfigStore(cfgPath).load();
        CHECK_EQ(reread.ramCacheSizeMb, 256, "ram_cache_size (user) round-trips through codec");
        CHECK_EQ(reread.diskCacheSizeMb, 2048, "disk_cache_size (user) round-trips through codec");
        CHECK(reread.cacheResponses && reread.cachePersist, "cache on/persist default true (not exposed to user)");

        setenv("DEED_RAM_CACHE_SIZE_MAX", "128", 1);
        setenv("DEED_DISK_CACHE_SIZE_MAX", "1024", 1);
        setenv("DEED_RAM_CACHE_THRESHOLD_KB", "256", 1);
        // detail::buildCacheConfig is the same builder CoreApiClient's native cache uses.
        CacheConfig cc = core::detail::buildCacheConfig(reread, core::CacheLimits{});
        CHECK_EQ(cc.ramEffBytes, (std::uint64_t)128 * 1024 * 1024, "ram clamped to env max 128MB");
        CHECK_EQ(cc.diskEffBytes, (std::uint64_t)1024 * 1024 * 1024, "disk clamped to env max 1024MB");
        CHECK_EQ(cc.thresholdBytes, (std::uint64_t)256 * 1024, "threshold = 256KB");

        unsetenv("DEED_RAM_CACHE_SIZE_MAX");
        unsetenv("DEED_DISK_CACHE_SIZE_MAX");
        unsetenv("DEED_RAM_CACHE_THRESHOLD_KB");
    }

    // (2) Via CacheLimits (.env loaded by UI): MIN floor raises low user; user in [min,max] unchanged.
    {
        AppConfig ac;
        ac.ramCacheSizeMb = 4;       // < min -> must raise to min
        ac.diskCacheSizeMb = 300;    // in [min,max] -> unchanged
        core::CacheLimits lim;
        lim.ramMinMb = 16; lim.ramMaxMb = 256;
        lim.diskMinMb = 64; lim.diskMaxMb = 1024;
        lim.thresholdKb = 128;
        CacheConfig cc = core::detail::buildCacheConfig(ac, lim);
        CHECK_EQ(cc.ramEffBytes, (std::uint64_t)16 * 1024 * 1024, "ram raised to min floor 16MB");
        CHECK_EQ(cc.diskEffBytes, (std::uint64_t)300 * 1024 * 1024, "disk (user) in [min,max] keeps 300MB");
        CHECK_EQ(cc.thresholdBytes, (std::uint64_t)128 * 1024, "threshold from .env = 128KB");
    }
}

static void test_app_config_defaults(const std::string& root) {
    std::printf("[app_config_defaults]\n");
    std::string cfgPath = (fs::path(root) / "appcfg_defaults.json").string();
    fs::remove(cfgPath);

    AppConfig defaults;
    defaults.fontName = "Courier";
    defaults.fontSize = 17;
    defaults.ramCacheSizeMb = 33;
    defaults.diskCacheSizeMb = 77;
    core::CacheLimits lim;
    lim.ramMinMb = 1; lim.ramMaxMb = 1000;
    lim.diskMinMb = 1; lim.diskMaxMb = 1000;

    AppConfigStore store(cfgPath);
    store.setDefaults(defaults);
    AppConfig c = store.load();
    CHECK_EQ(c.fontName, std::string("Courier"), "font_name default from .env");
    CHECK_EQ(c.fontSize, 17, "font_size default from .env");
    CHECK_EQ(c.ramCacheSizeMb, 33, "ram_cache_size default from .env");
    CHECK_EQ(c.diskCacheSizeMb, 77, "disk_cache_size default from .env");
    CHECK_EQ(core::detail::buildCacheConfig(c, lim).ramEffBytes, (std::uint64_t)33 * 1024 * 1024,
             "cache uses ram_cache_size default from .env");

    { std::ofstream o(cfgPath); o << "{\"font_size\": 20}"; }
    AppConfigStore st(cfgPath);
    st.setDefaults(defaults);
    AppConfig pc = st.load();
    CHECK_EQ(pc.fontSize, 20, "key present in file -> use file value");
    CHECK_EQ(pc.fontName, std::string("Courier"), "missing key -> falls back to .env default");
    CHECK_EQ(pc.ramCacheSizeMb, 33, "missing key -> ram default .env");
    fs::remove(cfgPath);
}

int run_cache_config_tests() {
    std::string root = makeTempRoot();

    test_response_cache(root);
    test_cache_durability(root);
    test_cache_config_clamp(root);
    test_app_config_defaults(root);

    fs::remove_all(root);
    std::printf("  cache_config: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
