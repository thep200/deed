// Unit test cho Core — chỉ dùng API public (include/core/). Không cần UI.
// Harness tối giản: đếm pass/fail, trả mã != 0 khi có lỗi (CTest đọc exit code).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "core/cache.hpp"
#include "core/engine.hpp"
#include "core/codec/field_codec.hpp"
#include "core/import_export/importer.hpp"
#include "core/persistence/request_naming.hpp"
#include "core/persistence/stores.hpp"
#include "core/variables/variable_resolver.hpp"

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

// Thư mục tạm cô lập cho mỗi run.
static std::string makeTempRoot() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = fs::temp_directory_path() / ("apiclient_test_" + std::to_string(stamp));
    fs::remove_all(base);
    fs::create_directories(base);
    return base.string();
}

// ---------------- VariableResolver ----------------
static void test_variable_resolver() {
    std::printf("[variable_resolver]\n");
    std::map<std::string, std::string> vars{{"baseUrl", "http://x"}, {"empty", ""}};

    auto r1 = VariableResolver::resolve("{{baseUrl}}/users", vars);
    CHECK_EQ(r1.text, std::string("http://x/users"), "resolve cơ bản");
    CHECK(r1.missing.empty(), "không có missing");

    auto r2 = VariableResolver::resolve("a{{empty}}b", vars);
    CHECK_EQ(r2.text, std::string("ab"), "biến rỗng -> \"\"");

    auto r3 = VariableResolver::resolve("x{{nope}}y", vars);
    CHECK_EQ(r3.text, std::string("x{{nope}}y"), "thiếu biến -> giữ literal");
    CHECK_EQ(r3.missing.size(), size_t(1), "ghi nhận 1 missing");

    auto r4 = VariableResolver::resolve("{{ baseUrl }}", vars);
    CHECK_EQ(r4.text, std::string("http://x"), "trim khoảng trắng trong {{ }}");
}

// ---------------- Request filename naming (LAZY_TREE §2) ----------------
static void test_request_naming() {
    std::printf("[request_naming]\n");

    // parse: gRPC = 2 phần (slug giữ '-'); HTTP = 3 phần (slug giữ '_').
    auto g = parseRequestFilename("grpc_get-list-user.json");
    CHECK(g.ok && g.type == RequestType::Grpc, "grpc parse ok");
    CHECK_EQ(g.slug, std::string("get-list-user"), "grpc slug = toàn bộ phần sau grpc_");
    CHECK(g.method.empty(), "grpc KHÔNG có method");

    auto h = parseRequestFilename("http_get_get_list_user.json");
    CHECK(h.ok && h.type == RequestType::Http, "http parse ok");
    CHECK_EQ(h.method, std::string("get"), "http method = phần 2");
    CHECK_EQ(h.slug, std::string("get_list_user"), "http slug giữ nguyên '_'");

    CHECK(!parseRequestFilename("collection.json").ok, "tên không đúng grammar -> ok=false");
    CHECK(!parseRequestFilename("README.md").ok, "không có '_' -> ok=false");

    // normalizeDisplayName: sentence-case, '_'/'-' -> space, bỏ ký tự đặc biệt.
    CHECK_EQ(normalizeDisplayName("get-list-user"), std::string("Get list user"), "de-slug grpc");
    CHECK_EQ(normalizeDisplayName("tours-configs"), std::string("Tours configs"), "de-slug http");
    CHECK_EQ(normalizeDisplayName("get_list_user"), std::string("Get list user"), "de-slug '_'");
    CHECK_EQ(normalizeDisplayName("name@of#request!"), std::string("Nameofrequest"),
             "ký tự đặc biệt bị loại");

    // encode: http có method, grpc KHÔNG; LỆNH §2.2 — không nhúng nguyên prefix vào label.
    CHECK_EQ(encodeRequestFilename(RequestType::Http, "POST", "Create Tour"),
             std::string("http_post_create-tour.json"), "encode http lowercase method");
    CHECK_EQ(encodeRequestFilename(RequestType::Grpc, "", "Get List User"),
             std::string("grpc_get-list-user.json"), "encode grpc KHÔNG có method");

    // round-trip: encode -> parse -> normalize KHÔNG còn prefix grpc_/http_<method>_.
    std::string fn = encodeRequestFilename(RequestType::Grpc, "", "Get List User");
    auto rt = parseRequestFilename(fn);
    std::string label = normalizeDisplayName(rt.slug);
    CHECK(label.find("grpc") == std::string::npos, "label KHÔNG chứa prefix 'grpc'");
    CHECK_EQ(label, std::string("Get list user"), "label = name đã chuẩn hoá");
}

// ---------------- CollectionStore round-trip + CRUD ----------------
static void test_collection_store(const std::string& root) {
    std::printf("[collection_store]\n");
    CollectionStore store(root);

    std::string rel = store.createRequest("", RequestType::Http, "Get Users");
    CHECK(!rel.empty(), "tạo request trả relPath");
    CHECK(fs::exists(fs::path(root) / rel), "file request tồn tại");

    RequestModel m = store.loadRequest(rel);
    CHECK_EQ(m.name, std::string("Get Users"), "name giữ nguyên");
    CHECK(m.type == RequestType::Http, "type = http");
    CHECK(!m.id.empty(), "có id");
    // id duy nhất + tìm theo id (sửa bug xoá nhầm do trùng id/đường dẫn).
    std::string r2 = store.createRequest("", RequestType::Http, "Another");
    RequestModel ma = store.loadRequest(r2);
    CHECK(!ma.id.empty() && ma.id != m.id, "2 request có id khác nhau");
    CHECK_EQ(store.findRelPathById(m.id), rel, "findRelPathById trả đúng path");
    CHECK(store.findRelPathById("req_nope").empty(), "id không có -> rỗng");
    store.remove(r2);

    CHECK(m.http.headers.size() >= 5, "HTTP request mới có header mặc định");
    bool hasContentType = false;
    for (const auto& h : m.http.headers) if (h.key == "Content-Type") hasContentType = true;
    CHECK(hasContentType, "có Content-Type mặc định");

    // sửa + lưu + đọc lại (round-trip). Đổi method -> tên file phải đổi http_get_* -> http_post_*.
    m.http.method = "POST";
    m.http.url = "{{baseUrl}}/users";
    m.http.body.mode = "json";
    m.http.body.json = "{\"a\":1}";
    std::string relAfterSave = store.saveRequest(rel, m);
    CHECK(!fs::exists(fs::path(root) / rel), "đổi method -> tên file cũ không còn");
    CHECK(fs::exists(fs::path(root) / relAfterSave), "tên file mới tồn tại sau save");
    CHECK(fs::path(relAfterSave).filename().string().rfind("http_post_", 0) == 0,
          "tên file phản ánh method mới (http_post_)");
    rel = relAfterSave;
    RequestModel m2 = store.loadRequest(rel);
    CHECK_EQ(m2.http.method, std::string("POST"), "method round-trip");
    CHECK_EQ(m2.http.url, std::string("{{baseUrl}}/users"), "url round-trip");
    CHECK_EQ(m2.http.body.json, std::string("{\"a\":1}"), "body json round-trip");

    // folder + request lồng.
    std::string folder = store.createFolder("", "Folder A");
    CHECK(fs::is_directory(fs::path(root) / folder), "folder tạo được");
    std::string nested = store.createRequest(folder, RequestType::Grpc, "Get User");
    RequestModel gm = store.loadRequest(nested);
    CHECK(gm.type == RequestType::Grpc, "nested = grpc");

    // tree.
    TreeNode tree = store.scanTree();
    CHECK(tree.isFolder, "root là folder");
    bool foundFolder = false;
    for (const auto& c : tree.children) if (c.isFolder && c.name == "folder-a") foundFolder = true;
    CHECK(foundFolder, "tree có folder con");

    // scanLevel: lazy 1 cấp — folder con KHÔNG nạp sẵn children; request leaf name đã chuẩn hoá.
    std::vector<TreeNode> rootLevel = store.scanLevel("");
    bool folderLazy = false, reqLabelOk = false;
    for (const auto& c : rootLevel) {
        if (c.isFolder && c.name == "folder-a") {
            folderLazy = c.children.empty();    // §3: folder fold, con rỗng tới khi expand
        } else if (!c.isFolder) {
            // rel hiện là http_post_get-users -> label "Get users", badge "POST".
            if (c.name == "Get users") { reqLabelOk = (c.methodOrType == "POST"); }
        }
    }
    CHECK(folderLazy, "scanLevel: folder con chưa nạp (lazy)");
    CHECK(reqLabelOk, "scanLevel: leaf name = de-slug, badge = method từ tên file");

    // scanLevel trong folder: grpc leaf, name de-slug, KHÔNG kèm prefix.
    std::vector<TreeNode> inFolder = store.scanLevel(folder);
    bool grpcLeafOk = false;
    for (const auto& c : inFolder)
        if (!c.isFolder && c.requestType == RequestType::Grpc && c.name == "Get user") grpcLeafOk = true;
    CHECK(grpcLeafOk, "scanLevel folder: grpc leaf name = 'Get user' (không prefix grpc_)");

    // move (drag-drop): chuyển request vào folder.
    std::string toMove = store.createRequest("", RequestType::Http, "Movable");
    std::string moved = store.move(toMove, folder);
    CHECK(!fs::exists(fs::path(root) / toMove), "file cũ không còn sau move");
    CHECK(fs::exists(fs::path(root) / moved), "file mới tồn tại trong folder");
    CHECK(moved.rfind("folder-a/", 0) == 0, "move đặt file vào folder đích");

    // duplicate + rename + remove.
    std::string dup = store.duplicate(rel);
    CHECK(fs::exists(fs::path(root) / dup), "duplicate tạo file");
    std::string renamed = store.rename(dup, "Renamed Req");
    CHECK(fs::exists(fs::path(root) / renamed), "rename tạo file mới");
    store.remove(renamed);
    CHECK(!fs::exists(fs::path(root) / renamed), "remove xoá file");

    // gitignore auto.
    store.ensureGitignore();
    std::string gi;
    fs::path gip = fs::path(root) / ".gitignore";
    CHECK(fs::exists(gip), ".gitignore tạo được");
}

// ---------------- SessionStore ----------------
static void test_session_store(const std::string& root) {
    std::printf("[session_store]\n");
    {
        SessionStore s(root);
        CHECK_EQ(s.getActiveEnv(), std::string("Global"), "active env mặc định Global");
        s.saveLastOpened("folderA/get.json");
        s.setActiveEnv("Dev");
    }
    {
        SessionStore s2(root); // đọc lại từ đĩa
        CHECK_EQ(s2.loadLastOpened(), std::string("folderA/get.json"), "lastOpened bền vững");
        CHECK_EQ(s2.getActiveEnv(), std::string("Dev"), "activeEnv bền vững");
    }
}

// ---------------- Secret + Environment ----------------
static void test_env_and_secret(const std::string& root) {
    std::printf("[environment + secret]\n");
    auto secrets = std::make_shared<FileSecretStore>(root);
    EnvironmentStore env(root, secrets);

    Environment dev;
    dev.name = "Dev";
    dev.keys.push_back({"baseUrl", "http://dev", false, true});
    dev.keys.push_back({"token", "s3cr3t", true, true}); // secret
    env.save(dev);

    auto names = env.list();
    bool hasDev = false;
    for (auto& n : names) if (n == "Dev") hasDev = true;
    CHECK(hasDev, "list() có Dev");

    Environment loaded = env.load("Dev");
    CHECK_EQ(loaded.keys.size(), size_t(2), "2 key");
    // secret value KHÔNG nằm trong file env nhưng load() bù lại từ SecretStore.
    std::string tokVal;
    bool tokSecret = false;
    for (auto& k : loaded.keys) if (k.key == "token") { tokVal = k.value; tokSecret = k.secret; }
    CHECK(tokSecret, "token đánh dấu secret");
    CHECK_EQ(tokVal, std::string("s3cr3t"), "secret value lấy từ SecretStore");

    // File env không được chứa giá trị secret.
    std::string envFileTxt;
    fs::path ef = fs::path(root) / "environments" / "Dev.json";
    std::FILE* f = std::fopen(ef.string().c_str(), "rb");
    if (f) { char buf[4096]; size_t n = std::fread(buf, 1, sizeof(buf), f); envFileTxt.assign(buf, n); std::fclose(f); }
    CHECK(envFileTxt.find("s3cr3t") == std::string::npos, "secret KHÔNG ghi vào file env");
    CHECK(fs::exists(fs::path(root) / ".secrets" / "secrets.json"), ".secrets/secrets.json tồn tại");

    secrets->remove("Dev", "token");
    CHECK_EQ(secrets->get("Dev", "token"), std::string(""), "remove secret");
}

// ---------------- Engine resolve + validate ----------------
static void test_engine(const std::string& root) {
    std::printf("[engine]\n");
    // chuẩn bị env Global + active.
    auto secrets = std::make_shared<FileSecretStore>(root);
    EnvironmentStore env(root, secrets);
    Environment g; g.name = "Global"; g.keys.push_back({"baseUrl", "http://global", false, true});
    env.save(g);
    Environment d; d.name = "Stage"; d.keys.push_back({"baseUrl", "http://stage", false, true});
    env.save(d);

    Engine engine(EngineConfig{root, (fs::path(root) / "appconfig.json").string()});
    engine.session().setActiveEnv("Stage");

    CHECK_EQ(engine.resolvePreview("{{baseUrl}}/x"), std::string("http://stage/x"),
             "active env override Global");
    engine.session().setActiveEnv("Global");
    CHECK_EQ(engine.resolvePreview("{{baseUrl}}/x"), std::string("http://global/x"),
             "fallback về Global");
    CHECK_EQ(engine.resolvePreview("{{missing}}"), std::string("{{missing}}"),
             "thiếu biến giữ literal");

    auto vok = engine.validateJson("{\"a\": 1}");
    CHECK(vok.ok, "JSON hợp lệ");
    auto vbad = engine.validateJson("{\"a\": }");
    CHECK(!vbad.ok, "JSON sai bị bắt");
    CHECK(vbad.line >= 1, "có vị trí lỗi");

    // resolveRequest áp env + app-global timeout.
    AppConfig ac; ac.defaultTimeoutMs = 12345; engine.appConfig().save(ac);
    RequestModel m; m.type = RequestType::Http; m.http.url = "{{baseUrl}}/u";
    auto rr = engine.resolveRequest(m);
    CHECK_EQ(rr.model.http.url, std::string("http://global/u"), "resolveRequest resolve url");
    CHECK_EQ(rr.model.http.settings.timeoutMs, 12345, "timeout lấy từ app-global khi chưa set");
}

// ---------------- ResponseCache (RESPONSE_CACHE.md §10) ----------------
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
    cfg.ramEffBytes = 4 * 1024;      // RAM cap nhỏ để kiểm LRU
    cfg.diskEffBytes = 1024 * 1024;
    cfg.thresholdBytes = 1024;       // < 1KB -> ưu tiên RAM
    cfg.enabled = true;
    cfg.persist = true;
    std::string sdir = (fs::path(root) / ".session_cache_test").string();
    fs::remove_all(sdir);
    auto cache = ResponseCache::create(cfg, sdir);

    // small (< threshold) -> RAM + write-through disk.
    cache->put("a", mkRec(100, 200));
    CHECK(cache->l1UsedBytes() > 0, "response nhỏ vào RAM");
    CHECK(cache->l2UsedBytes() > 0, "response nhỏ write-through disk");
    auto ga = cache->get("a");
    CHECK(ga && ga->response.statusCode == 200, "get a trúng L1");

    // large (>= threshold) -> disk only, KHÔNG vào RAM.
    std::uint64_t l1Before = cache->l1UsedBytes();
    cache->put("big", mkRec(4000, 200));
    CHECK_EQ(cache->l1UsedBytes(), l1Before, "response lớn KHÔNG vào RAM");
    auto gb = cache->get("big");
    CHECK(gb && gb->response.body.size() == 4000, "get big trúng (từ disk)");

    // LRU bound RAM: nhồi nhiều record nhỏ -> không vượt cap.
    for (int i = 0; i < 50; i++) cache->put("k" + std::to_string(i), mkRec(200, 200));
    CHECK(cache->l1UsedBytes() <= cfg.ramEffBytes, "RAM cache không vượt cap (LRU evict)");

    // remove -> miss cả 2 tầng.
    cache->remove("big");
    CHECK(!cache->get("big"), "remove -> miss");

    // restart: L1 trống, đọc lại từ disk; disk-hit nhỏ -> promote lên L1.
    cache->put("small2", mkRec(50, 201));
    cache.reset();
    auto cache2 = ResponseCache::create(cfg, sdir);
    CHECK_EQ(cache2->l1UsedBytes(), (std::uint64_t)0, "restart: L1 trống");
    auto gs = cache2->get("small2");
    CHECK(gs && gs->response.statusCode == 201, "restart: đọc small2 từ disk");
    CHECK(cache2->l1UsedBytes() > 0, "disk-hit nhỏ -> promote lên RAM");

    // Driver giả: chứng minh trừu tượng hoá — facade chạy với ICacheDriver bất kỳ, không sửa ResponseCache.
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
    CHECK(!nullCache.get("x"), "NullCacheDriver cắm được vào facade (không đụng ResponseCache)");

    fs::remove_all(sdir);
}

// ENV trần/sàn kẹp user; user config được đọc đúng (RESPONSE_CACHE.md §1.2 + nghiệm thu §10).
static void test_cache_config_clamp(const std::string& root) {
    std::printf("[cache_config]\n");

    // (1) Qua getenv (ops): user > max -> kẹp về max.
    {
        std::string cfgPath = (fs::path(root) / "appcfg_cache.json").string();
        AppConfig ac;
        ac.ramCacheSizeMb = 256;     // user > env max -> phải kẹp
        ac.diskCacheSizeMb = 2048;
        AppConfigStore(cfgPath).save(ac);
        // đọc lại từ file -> đảm bảo key snake_case round-trip + user value được nạp.
        AppConfig reread = AppConfigStore(cfgPath).load();
        CHECK_EQ(reread.ramCacheSizeMb, 256, "ram_cache_size (user) round-trip qua codec");
        CHECK_EQ(reread.diskCacheSizeMb, 2048, "disk_cache_size (user) round-trip qua codec");
        CHECK(reread.cacheResponses && reread.cachePersist, "cache on/persist mặc định true (không phơi user)");

        setenv("DEED_RAM_CACHE_SIZE_MAX", "128", 1);
        setenv("DEED_DISK_CACHE_SIZE_MAX", "1024", 1);
        setenv("DEED_RAM_CACHE_THRESHOLD_KB", "256", 1);
        EngineConfig ecfg;
        ecfg.collectionRoot = (fs::path(root) / "cache_cfg_root").string();
        ecfg.appConfigPath = cfgPath;
        Engine eng(ecfg);
        const CacheConfig& cc = eng.cacheConfig();
        CHECK_EQ(cc.ramEffBytes, (std::uint64_t)128 * 1024 * 1024, "ram kẹp về env max 128MB");
        CHECK_EQ(cc.diskEffBytes, (std::uint64_t)1024 * 1024 * 1024, "disk kẹp về env max 1024MB");
        CHECK_EQ(cc.thresholdBytes, (std::uint64_t)256 * 1024, "threshold = 256KB");

        ApiResponse resp; resp.statusCode = 204; resp.body = "ok";
        eng.putResponse("req_X", resp);
        auto got = eng.getResponse("req_X");
        CHECK(got && got->response.statusCode == 204, "Engine putResponse/getResponse round-trip");
        eng.removeResponse("req_X");
        CHECK(!eng.getResponse("req_X"), "Engine removeResponse -> miss");

        unsetenv("DEED_RAM_CACHE_SIZE_MAX");
        unsetenv("DEED_DISK_CACHE_SIZE_MAX");
        unsetenv("DEED_RAM_CACHE_THRESHOLD_KB");
    }

    // (2) Qua CacheLimits (.env do UI nạp): sàn MIN nâng user thấp; user trong [min,max] giữ nguyên.
    {
        std::string cfgPath = (fs::path(root) / "appcfg_cache2.json").string();
        AppConfig ac;
        ac.ramCacheSizeMb = 4;       // < min -> phải nâng lên min
        ac.diskCacheSizeMb = 300;    // trong [min,max] -> giữ nguyên
        AppConfigStore(cfgPath).save(ac);

        EngineConfig ecfg;
        ecfg.collectionRoot = (fs::path(root) / "cache_cfg_root2").string();
        ecfg.appConfigPath = cfgPath;
        ecfg.cacheLimits.ramMinMb = 16; ecfg.cacheLimits.ramMaxMb = 256;
        ecfg.cacheLimits.diskMinMb = 64; ecfg.cacheLimits.diskMaxMb = 1024;
        ecfg.cacheLimits.thresholdKb = 128;
        Engine eng(ecfg);
        const CacheConfig& cc = eng.cacheConfig();
        CHECK_EQ(cc.ramEffBytes, (std::uint64_t)16 * 1024 * 1024, "ram nâng lên sàn min 16MB");
        CHECK_EQ(cc.diskEffBytes, (std::uint64_t)300 * 1024 * 1024, "disk (user) trong [min,max] giữ 300MB");
        CHECK_EQ(cc.thresholdBytes, (std::uint64_t)128 * 1024, "threshold từ .env = 128KB");
    }
}

// App-config defaults từ .env (EngineConfig.appDefaults) khi config.json thiếu key.
static void test_app_config_defaults(const std::string& root) {
    std::printf("[app_config_defaults]\n");
    std::string cfgPath = (fs::path(root) / "appcfg_defaults.json").string();
    fs::remove(cfgPath);

    EngineConfig ec;
    ec.collectionRoot = (fs::path(root) / "defs_root").string();
    ec.appConfigPath = cfgPath;
    ec.appDefaults.defaultTimeoutMs = 12345;
    ec.appDefaults.verifyTls = false;
    ec.appDefaults.fontName = "Courier";
    ec.appDefaults.fontSize = 17;
    ec.appDefaults.ramCacheSizeMb = 33;
    ec.appDefaults.diskCacheSizeMb = 77;
    ec.cacheLimits.ramMinMb = 1; ec.cacheLimits.ramMaxMb = 1000;
    ec.cacheLimits.diskMinMb = 1; ec.cacheLimits.diskMaxMb = 1000;
    Engine eng(ec);

    // Chưa có config.json -> load trả defaults (.env).
    AppConfig c = eng.appConfig().load();
    CHECK_EQ(c.defaultTimeoutMs, 12345, "default_timeout_ms từ .env khi chưa có config");
    CHECK(!c.verifyTls, "verify_tls default từ .env");
    CHECK_EQ(c.fontName, std::string("Courier"), "font_name default từ .env");
    CHECK_EQ(c.fontSize, 17, "font_size default từ .env");
    CHECK_EQ(c.ramCacheSizeMb, 33, "ram_cache_size default từ .env");
    CHECK_EQ(c.diskCacheSizeMb, 77, "disk_cache_size default từ .env");
    CHECK_EQ(eng.cacheConfig().ramEffBytes, (std::uint64_t)33 * 1024 * 1024,
             "cache dùng ram_cache_size default từ .env");

    // config.json chỉ có 1 key -> các key thiếu rơi về defaults (.env).
    { std::ofstream o(cfgPath); o << "{\"font_size\": 20}"; }
    AppConfigStore st(cfgPath);
    st.setDefaults(ec.appDefaults);
    AppConfig pc = st.load();
    CHECK_EQ(pc.fontSize, 20, "key có trong file -> dùng giá trị file");
    CHECK_EQ(pc.defaultTimeoutMs, 12345, "key thiếu -> rơi về default .env");
    CHECK_EQ(pc.ramCacheSizeMb, 33, "key thiếu -> ram default .env");
    fs::remove(cfgPath);
}

// ---------------- Importers ----------------
static void test_importers() {
    std::printf("[importers]\n");
    CurlImporter curl;
    CHECK(curl.canHandle("curl http://x"), "canHandle curl");
    CHECK(!curl.canHandle("wget http://x"), "không nhận wget");

    auto r = curl.parse("curl -X POST 'http://api.test/users?q=1' "
                        "-H 'Content-Type: application/json' "
                        "-H 'Authorization: Bearer abc' "
                        "-d '{\"name\":\"Alice\"}'");
    CHECK(r.ok, "parse curl ok");
    CHECK_EQ(r.model.http.method, std::string("POST"), "method POST");
    CHECK_EQ(r.model.http.url, std::string("http://api.test/users?q=1"), "url giữ nguyên");
    CHECK_EQ(r.model.http.body.mode, std::string("json"), "body json từ content-type");
    CHECK_EQ(r.model.http.body.json, std::string("{\"name\":\"Alice\"}"), "body content");
    CHECK_EQ(r.model.http.headers.size(), size_t(2), "2 headers");

    auto rb = curl.parse("curl -u user:pass http://api.test/secure");
    CHECK_EQ(rb.model.http.auth.type, std::string("basic"), "-u -> basic auth");
    CHECK_EQ(rb.model.http.auth.basicUsername, std::string("user"), "basic user");
    CHECK_EQ(rb.model.http.method, std::string("GET"), "không body -> GET");

    GrpcImporter g;
    CHECK(g.canHandle("grpcurl -plaintext localhost:50051 pkg.Svc/M"), "canHandle grpcurl");
    auto gr = g.parse("grpcurl -plaintext -d '{\"id\":\"1\"}' -H 'authorization: Bearer t' "
                      "localhost:50051 user.v1.UserService/GetUser");
    CHECK(gr.ok, "parse grpcurl ok");
    CHECK_EQ(gr.model.grpc.target, std::string("localhost:50051"), "target");
    CHECK_EQ(gr.model.grpc.service, std::string("user.v1.UserService"), "service");
    CHECK_EQ(gr.model.grpc.method, std::string("GetUser"), "method");
    CHECK_EQ(gr.model.grpc.tls.enabled, false, "-plaintext -> tls off");
    CHECK_EQ(gr.model.grpc.metadata.size(), size_t(1), "1 metadata");

    auto gr2 = g.parse("grpcs://localhost:50051/pkg.Service/Method");
    CHECK(gr2.ok, "parse chuỗi gọn ok");
    CHECK_EQ(gr2.model.grpc.tls.enabled, true, "grpcs -> tls on");
    CHECK_EQ(gr2.model.grpc.service, std::string("pkg.Service"), "service từ chuỗi gọn");
}

static void test_field_codec() {
    std::printf("[field_codec]\n");
    std::string body = "{\"a\":1}";
    CHECK(fieldcodec::formatJson(body, true).find('\n') != std::string::npos, "pretty có xuống dòng");
    CHECK_EQ(fieldcodec::formatJson(body, false), std::string("{\"a\":1}"), "compact bỏ khoảng trắng");
    std::string enc = fieldcodec::jsonEncodeString(body);
    CHECK(enc.front() == '"' && enc.back() == '"', "encode -> string literal có nháy");
    CHECK_EQ(fieldcodec::jsonDecodeString(enc), body, "decode(encode(x)) == x");
}

static void test_curl_export() {
    std::printf("[curl_export]\n");
    RequestModel m;
    m.type = RequestType::Http;
    m.http.method = "POST";
    m.http.url = "https://api.test/users";
    m.http.headers.push_back({"Content-Type", "application/json", true});
    m.http.headers.push_back({"X-Token", "abc123", true});
    m.http.params.push_back({"q", "hello", true});
    m.http.body.mode = "json";
    m.http.body.json = "{\"a\":1}";
    std::string c = toCurl(m);
    CHECK(c.find("curl -X POST") != std::string::npos, "có method");
    CHECK(c.find("api.test/users") != std::string::npos, "có url");
    CHECK(c.find("--data") != std::string::npos, "có body");
    CHECK(c.find("X-Token: abc123") != std::string::npos, "có header X-Token");
    CHECK(c.find("q=hello") != std::string::npos, "có param q");

    GrpcRequest& g = m.grpc;
    m.type = RequestType::Grpc;
    g.target = "localhost:50051"; g.service = "pkg.Svc"; g.method = "M"; g.message = "{\"id\":\"1\"}";
    std::string gc = toCurl(m);
    CHECK(gc.find("grpcurl") != std::string::npos, "grpc -> grpcurl");
    CHECK(gc.find("pkg.Svc/M") != std::string::npos, "có service/method");
}

int main() {
    std::string root = makeTempRoot();
    std::printf("Temp root: %s\n", root.c_str());

    test_variable_resolver();
    test_field_codec();
    test_curl_export();
    test_request_naming();
    test_response_cache(root);
    test_cache_config_clamp(root);
    test_app_config_defaults(root);
    test_collection_store(root);
    test_session_store(root);
    test_env_and_secret(root);
    test_engine(root);
    test_importers();

    fs::remove_all(root);

    std::printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
