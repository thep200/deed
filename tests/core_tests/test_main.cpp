// Unit test cho Core — chỉ dùng API public (include/core/). Không cần UI.
// Harness tối giản: đếm pass/fail, trả mã != 0 khi có lỗi (CTest đọc exit code).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

#include "core/engine.hpp"
#include "core/field_codec.hpp"
#include "core/importer.hpp"
#include "core/stores.hpp"
#include "core/variable_resolver.hpp"

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

    // sửa + lưu + đọc lại (round-trip).
    m.http.method = "POST";
    m.http.url = "{{baseUrl}}/users";
    m.http.body.mode = "json";
    m.http.body.json = "{\"a\":1}";
    store.saveRequest(rel, m);
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
    test_collection_store(root);
    test_session_store(root);
    test_env_and_secret(root);
    test_engine(root);
    test_importers();

    fs::remove_all(root);

    std::printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
