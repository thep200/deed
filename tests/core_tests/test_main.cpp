// Unit tests for Core — public API only (include/core/). No UI needed.
// Minimal harness: count pass/fail, return code != 0 on error (CTest reads exit code).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/engine.hpp"
#include "core/codec/field_codec.hpp"
#include "core/import_export/importer.hpp"
#include "core/persistence/request_naming.hpp"
#include "core/persistence/stores.hpp"
#include "core/variables/variable_resolver.hpp"

namespace fs = std::filesystem;
using namespace core;

// Defined in stream_sink_test.cpp — gatekeeper for INV-1 (SPEC_grpc_streaming AC-4). Returns #failures.
int run_stream_sink_tests();
// Defined in ws_session_test.cpp — gatekeeper for INV-1 duplex (SPEC_websocket AC-6/AC-7). Returns #failures.
int run_ws_session_tests();
// Defined in sse_parser_test.cpp — SSE wire parser gatekeeper (SPEC_sse AC-2/3/5/7). Returns #failures.
int run_sse_parser_tests();
// Defined in gql_ws_protocol_test.cpp — GraphQL-over-WS protocol gatekeeper (SPEC_graphql AC-7). Returns #failures.
int run_gql_ws_protocol_tests();

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

// Isolated temp directory for each run.
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
    CHECK_EQ(r1.text, std::string("http://x/users"), "basic resolve");
    CHECK(r1.missing.empty(), "no missing");

    auto r2 = VariableResolver::resolve("a{{empty}}b", vars);
    CHECK_EQ(r2.text, std::string("ab"), "empty var -> \"\"");

    auto r3 = VariableResolver::resolve("x{{nope}}y", vars);
    CHECK_EQ(r3.text, std::string("x{{nope}}y"), "missing var -> keep literal");
    CHECK_EQ(r3.missing.size(), size_t(1), "record 1 missing");

    auto r4 = VariableResolver::resolve("{{ baseUrl }}", vars);
    CHECK_EQ(r4.text, std::string("http://x"), "trim whitespace inside {{ }}");
}

// ---------------- VariableResolver: reverse substitution (value -> {{alias}}) ----------------
static void test_alias_inversion() {
    std::printf("[alias_inversion]\n");
    // (key,value) in env definition order. "zfirst" comes BEFORE "afirst" although it sorts later
    // -> lets us prove the FIRST-defined key (not the lexicographically smallest) wins a tie.
    std::vector<std::pair<std::string, std::string>> vars{
        {"baseUrl", "https://api.example.com"},
        {"token", "secretXYZ"},
        {"empty", ""},
        {"zfirst", "shared"},
        {"afirst", "shared"},
    };
    std::string out, key;

    // whole-value exact match -> {{key}}
    CHECK(VariableResolver::valueToAlias("secretXYZ", vars, out, &key), "whole match found");
    CHECK_EQ(out, std::string("{{token}}"), "whole -> {{token}}");
    CHECK_EQ(key, std::string("token"), "whole reports key");

    // no match -> unchanged / false
    CHECK(!VariableResolver::valueToAlias("not-in-env", vars, out), "whole no match");
    // empty env value never matches an empty field (avoids aliasing everything)
    CHECK(!VariableResolver::valueToAlias("", vars, out), "empty field no match");

    // duplicate values -> the FIRST-defined key wins (zfirst before afirst), NOT smallest key.
    CHECK(VariableResolver::valueToAlias("shared", vars, out, &key), "tie match");
    CHECK_EQ(key, std::string("zfirst"), "tie -> first-defined key");

    // prefix match (baseUrl pattern), remainder preserved
    CHECK(VariableResolver::prefixToAlias("https://api.example.com/v1/users", vars, out, &key),
          "prefix match found");
    CHECK_EQ(out, std::string("{{baseUrl}}/v1/users"), "prefix -> {{baseUrl}}/rest");

    // prefix exact (whole url == baseUrl) -> just the alias
    CHECK(VariableResolver::prefixToAlias("https://api.example.com", vars, out),
          "prefix exact match");
    CHECK_EQ(out, std::string("{{baseUrl}}"), "prefix exact -> {{baseUrl}}");

    // duplicate prefix values of equal length -> first-defined wins (host1 before host2).
    std::vector<std::pair<std::string, std::string>> hosts{
        {"host1", "http://dup.local"}, {"host2", "http://dup.local"}};
    CHECK(VariableResolver::prefixToAlias("http://dup.local/x", hosts, out, &key), "dup prefix match");
    CHECK_EQ(key, std::string("host1"), "dup prefix -> first-defined key");

    // already aliased -> idempotent (no match)
    CHECK(!VariableResolver::prefixToAlias("{{baseUrl}}/v1", vars, out), "no re-alias of {{ }}");

    // short value below the prefix floor must NOT mangle text
    std::vector<std::pair<std::string, std::string>> shortVars{{"x", "ht"}};
    CHECK(!VariableResolver::prefixToAlias("http://h", shortVars, out), "short prefix ignored");
}

// ---------------- Request filename naming (new_format.file.md §2A: id FIRST) ----------------
static void test_request_naming() {
    std::printf("[request_naming]\n");

    // NEW parse: <id>_<type>_... — first token = id; slug keeps '_'/'-'.
    auto g = parseRequestFilename("ab12cd_grpc_get-list-user.json");
    CHECK(g.ok && g.type == RequestType::Grpc, "grpc parse ok");
    CHECK_EQ(g.id, std::string("ab12cd"), "id = first token");
    CHECK_EQ(g.slug, std::string("get-list-user"), "grpc slug = part after grpc_");
    CHECK(g.method.empty(), "grpc has NO method");

    auto h = parseRequestFilename("xy9z_http_get_get_list_user.json");
    CHECK(h.ok && h.type == RequestType::Http, "http parse ok");
    CHECK_EQ(h.id, std::string("xy9z"), "id = first token");
    CHECK_EQ(h.method, std::string("get"), "http method");
    CHECK_EQ(h.slug, std::string("get_list_user"), "http slug keeps '_'");

    // BACK-COMPAT: OLD file with no id (first token = http/grpc) -> parses, id empty.
    auto old = parseRequestFilename("http_get_tours-configs.json");
    CHECK(old.ok && old.id.empty() && old.type == RequestType::Http, "old file: id empty, still parses");
    CHECK_EQ(old.slug, std::string("tours-configs"), "old file slug correct");

    CHECK(!parseRequestFilename("collection.json").ok, "name not matching grammar -> ok=false");
    CHECK(!parseRequestFilename("README.md").ok, "no '_' -> ok=false");
    CHECK(!parseRequestFilename("ab12_xxx_slug.json").ok, "unknown type -> ok=false");

    // isValidFileId: only [a-z0-9], no '_'/uppercase.
    CHECK(isValidFileId("ab12cd34"), "valid base36 id");
    CHECK(!isValidFileId("req_ABC"), "legacy id with '_'/uppercase -> invalid");
    CHECK(!isValidFileId(""), "empty id -> invalid");

    // normalizeDisplayName: sentence-case, '_'/'-' -> space, drop special chars.
    CHECK_EQ(normalizeDisplayName("get-list-user"), std::string("Get list user"), "de-slug grpc");
    CHECK_EQ(normalizeDisplayName("get_list_user"), std::string("Get list user"), "de-slug '_'");
    CHECK_EQ(normalizeDisplayName("name@of#request!"), std::string("Nameofrequest"),
             "special chars dropped");

    // encode: <id> first; http has method, grpc does NOT.
    CHECK_EQ(encodeRequestFilename("k7id", RequestType::Http, "POST", "Create Tour"),
             std::string("k7id_http_post_create-tour.json"), "encode http: id + method");
    CHECK_EQ(encodeRequestFilename("k7id", RequestType::Grpc, "", "Get List User"),
             std::string("k7id_grpc_get-list-user.json"), "encode grpc: id, NO method");

    // round-trip: encode -> parse keeps id; label = normalized slug, NO id/prefix.
    std::string fn = encodeRequestFilename("zz9", RequestType::Grpc, "", "Get List User");
    auto rt = parseRequestFilename(fn);
    CHECK_EQ(rt.id, std::string("zz9"), "round-trip keeps id");
    std::string label = normalizeDisplayName(rt.slug);
    CHECK(label.find("grpc") == std::string::npos && label.find("zz9") == std::string::npos,
          "label does NOT contain id/prefix");
    CHECK_EQ(label, std::string("Get list user"), "label = normalized name");
}

// Migration: OLD file (no id in name) -> add id to name (git mv), zero-read next time.
static void test_filename_migration(const std::string& root) {
    std::printf("[filename_migration]\n");
    std::string mroot = (fs::path(root) / "migrate_root").string();
    fs::create_directories(mroot);
    CollectionStore store(mroot);

    std::string oldName = "http_get_legacy-req.json";   // old form, legacy id in content
    { std::ofstream o((fs::path(mroot) / oldName));
      o << R"({"schemaVersion":1,"id":"req_OLD","name":"Legacy Req","type":"http","http":{"method":"GET","url":""}})"; }

    int n = store.migrateAddIdToFilenames();
    CHECK(n >= 1, "migrate renames >=1 old file");
    CHECK(!fs::exists(fs::path(mroot) / oldName), "old name gone after migrate");

    std::vector<TreeNode> lvl = store.scanLevel("");
    bool ok = false; std::string newId;
    for (auto& c : lvl)
        if (!c.isFolder) { ok = !c.id.empty() && c.name == "Legacy req"; newId = c.id; }
    CHECK(ok, "scanLevel: id from FILENAME (zero-read), name normalized");
    CHECK(isValidFileId(newId), "new id valid [a-z0-9], no '_'");
    CHECK_EQ(store.migrateAddIdToFilenames(), 0, "migrate 2nd time: 0 (already has id, no content read)");
    CHECK(!store.findRelPathById(newId).empty(), "findRelPathById by id from filename");
}

// ---------------- CollectionStore round-trip + CRUD ----------------
static void test_collection_store(const std::string& root) {
    std::printf("[collection_store]\n");
    CollectionStore store(root);

    std::string rel = store.createRequest("", RequestType::Http, "Get Users");
    CHECK(!rel.empty(), "create request returns relPath");
    CHECK(fs::exists(fs::path(root) / rel), "request file exists");

    RequestModel m = store.loadRequest(rel);
    CHECK_EQ(m.name, std::string("Get Users"), "name preserved");
    CHECK(m.type == RequestType::Http, "type = http");
    CHECK(!m.id.empty(), "has id");
    // unique id + find by id (fixes bug deleting wrong file due to duplicate id/path).
    std::string r2 = store.createRequest("", RequestType::Http, "Another");
    RequestModel ma = store.loadRequest(r2);
    CHECK(!ma.id.empty() && ma.id != m.id, "2 requests have different ids");
    CHECK_EQ(store.findRelPathById(m.id), rel, "findRelPathById returns correct path");
    CHECK(store.findRelPathById("req_nope").empty(), "missing id -> empty");
    store.remove(r2);

    CHECK(m.http.headers.size() >= 5, "new HTTP request has default headers");
    bool hasContentType = false;
    for (const auto& h : m.http.headers) if (h.key == "Content-Type") hasContentType = true;
    CHECK(hasContentType, "has default Content-Type");
    // New requests auto-identify with an ENABLED User-Agent="deed"; the other hint headers stay off.
    bool uaOk = false, ctEnabled = false;
    for (const auto& h : m.http.headers) {
        if (h.key == "User-Agent") uaOk = (h.value == "deed" && h.enabled);
        if (h.key == "Content-Type") ctEnabled = h.enabled;
    }
    CHECK(uaOk, "User-Agent=deed enabled on new request");
    CHECK(!ctEnabled, "other hint headers remain disabled");

    // edit + save + reload (round-trip). Change method -> filename must change http_get_* -> http_post_*.
    m.http.method = "POST";
    m.http.url = "{{baseUrl}}/users";
    m.http.body.mode = "json";
    m.http.body.json = "{\"a\":1}";
    std::string relAfterSave = store.saveRequest(rel, m);
    CHECK(!fs::exists(fs::path(root) / rel), "change method -> old filename gone");
    CHECK(fs::exists(fs::path(root) / relAfterSave), "new filename exists after save");
    {
        std::string fn = fs::path(relAfterSave).filename().string();
        CHECK(fn.find("_http_post_") != std::string::npos, "filename reflects new method (http_post)");
        core::ParsedRequestName pr = core::parseRequestFilename(fn);
        CHECK(pr.ok && core::isValidFileId(pr.id), "new filename has valid id at front");
    }
    rel = relAfterSave;
    RequestModel m2 = store.loadRequest(rel);
    CHECK_EQ(m2.http.method, std::string("POST"), "method round-trip");
    CHECK_EQ(m2.http.url, std::string("{{baseUrl}}/users"), "url round-trip");
    CHECK_EQ(m2.http.body.json, std::string("{\"a\":1}"), "body json round-trip");

    // folder + nested request.
    std::string folder = store.createFolder("", "Folder A");
    CHECK(fs::is_directory(fs::path(root) / folder), "folder created");
    std::string nested = store.createRequest(folder, RequestType::Grpc, "Get User");
    RequestModel gm = store.loadRequest(nested);
    CHECK(gm.type == RequestType::Grpc, "nested = grpc");

    // tree.
    TreeNode tree = store.scanTree();
    CHECK(tree.isFolder, "root is a folder");
    bool foundFolder = false;
    for (const auto& c : tree.children) if (c.isFolder && c.name == "folder-a") foundFolder = true;
    CHECK(foundFolder, "tree has child folder");

    // scanLevel: lazy 1 level — child folders do NOT preload children; request leaf name normalized.
    std::vector<TreeNode> rootLevel = store.scanLevel("");
    bool folderLazy = false, reqLabelOk = false;
    for (const auto& c : rootLevel) {
        if (c.isFolder && c.name == "folder-a") {
            folderLazy = c.children.empty();    // §3: folder folded, children empty until expand
        } else if (!c.isFolder) {
            // rel is now http_post_get-users -> label "Get users", badge "POST".
            if (c.name == "Get users") { reqLabelOk = (c.methodOrType == "POST"); }
        }
    }
    CHECK(folderLazy, "scanLevel: child folder not loaded (lazy)");
    CHECK(reqLabelOk, "scanLevel: leaf name = de-slug, badge = method from filename");

    // scanLevel inside folder: grpc leaf, de-slug name, NO prefix.
    std::vector<TreeNode> inFolder = store.scanLevel(folder);
    bool grpcLeafOk = false;
    for (const auto& c : inFolder)
        if (!c.isFolder && c.requestType == RequestType::Grpc && c.name == "Get user") grpcLeafOk = true;
    CHECK(grpcLeafOk, "scanLevel folder: grpc leaf name = 'Get user' (no grpc_ prefix)");

    // move (drag-drop): move request into folder.
    std::string toMove = store.createRequest("", RequestType::Http, "Movable");
    std::string moved = store.move(toMove, folder);
    CHECK(!fs::exists(fs::path(root) / toMove), "old file gone after move");
    CHECK(fs::exists(fs::path(root) / moved), "new file exists in folder");
    CHECK(moved.rfind("folder-a/", 0) == 0, "move places file into target folder");

    // duplicate + rename + remove.
    std::string dup = store.duplicate(rel);
    CHECK(fs::exists(fs::path(root) / dup), "duplicate creates file");
    std::string renamed = store.rename(dup, "Renamed Req");
    CHECK(fs::exists(fs::path(root) / renamed), "rename creates new file");
    store.remove(renamed);
    CHECK(!fs::exists(fs::path(root) / renamed), "remove deletes file");

    // gitignore auto.
    store.ensureGitignore();
    std::string gi;
    fs::path gip = fs::path(root) / ".gitignore";
    CHECK(fs::exists(gip), ".gitignore created");
}

// ---------------- SessionStore ----------------
static void test_session_store(const std::string& root) {
    std::printf("[session_store]\n");
    {
        SessionStore s(root);
        CHECK_EQ(s.getActiveEnv(), std::string("Global"), "active env defaults to Global");
        s.saveLastOpened("folderA/get.json");
        s.setActiveEnv("Dev");
    }
    {
        SessionStore s2(root); // reload from disk
        CHECK_EQ(s2.loadLastOpened(), std::string("folderA/get.json"), "lastOpened persists");
        CHECK_EQ(s2.getActiveEnv(), std::string("Dev"), "activeEnv persists");
    }
}

// ---------------- Environment (plaintext) + rename + migration ----------------
static void test_env_and_secret(const std::string& root) {
    std::printf("[environment]\n");
    EnvironmentStore env(root);

    Environment dev;
    dev.name = "Dev";
    dev.keys.push_back({"baseUrl", "http://dev", true});
    dev.keys.push_back({"token", "s3cr3t", true});
    env.save(dev);

    auto names = env.list();
    bool hasDev = false;
    for (auto& n : names) if (n == "Dev") hasDev = true;
    CHECK(hasDev, "list() has Dev");

    Environment loaded = env.load("Dev");
    CHECK_EQ(loaded.keys.size(), size_t(2), "2 keys");
    // Plaintext value lives directly in the env file (no more SecretStore).
    std::string tokVal;
    for (auto& k : loaded.keys) if (k.key == "token") tokVal = k.value;
    CHECK_EQ(tokVal, std::string("s3cr3t"), "value read directly from env file");

    std::string envFileTxt;
    fs::path ef = fs::path(root) / "environments" / "Dev.json";
    std::FILE* f = std::fopen(ef.string().c_str(), "rb");
    if (f) { char buf[4096]; size_t n = std::fread(buf, 1, sizeof(buf), f); envFileTxt.assign(buf, n); std::fclose(f); }
    CHECK(envFileTxt.find("s3cr3t") != std::string::npos, "value written plaintext to env file");
    CHECK(!fs::exists(fs::path(root) / ".secrets"), "no .secrets/ created");

    // renameEnv: rename file, content preserved.
    CHECK(env.renameEnv("Dev", "Dev2"), "renameEnv succeeds");
    CHECK(!fs::exists(ef), "old env file gone");
    CHECK_EQ(env.load("Dev2").keys.size(), size_t(2), "new env keeps keys");
    CHECK(!env.renameEnv("Dev2", ""), "empty renameEnv rejected");

    // renameAlias: rename key across all envs.
    Environment stg; stg.name = "Stg"; stg.keys.push_back({"baseUrl", "http://stg", true});
    env.save(stg);
    CHECK(env.renameAlias("baseUrl", "apiUrl"), "renameAlias succeeds");
    bool found = false;
    for (auto& k : env.load("Dev2").keys) if (k.key == "apiUrl") found = true;
    CHECK(found, "alias renamed on Dev2");
    found = false;
    for (auto& k : env.load("Stg").keys) if (k.key == "apiUrl") found = true;
    CHECK(found, "alias renamed on Stg simultaneously");
}

// ---------------- Migration secret -> plaintext (idempotent) ----------------
static void test_secret_migration(const std::string& root) {
    std::printf("[secret migration]\n");
    // Build OLD state: env file missing value + .secrets/secrets.json holds value.
    fs::create_directories(fs::path(root) / "environments");
    fs::create_directories(fs::path(root) / ".secrets");
    {
        std::FILE* f = std::fopen((fs::path(root) / "environments" / "Dev.json").string().c_str(), "wb");
        const char* env = "{\"schemaVersion\":1,\"name\":\"Dev\",\"keys\":[{\"key\":\"token\",\"enabled\":true}]}";
        std::fwrite(env, 1, std::strlen(env), f); std::fclose(f);
    }
    {
        std::FILE* f = std::fopen((fs::path(root) / ".secrets" / "secrets.json").string().c_str(), "wb");
        const char* sec = "{\"Dev\":{\"token\":\"s3cr3t\"}}";
        std::fwrite(sec, 1, std::strlen(sec), f); std::fclose(f);
    }
    EnvironmentStore env(root);
    env.migrateLegacySecrets();
    std::string tokVal;
    for (auto& k : env.load("Dev").keys) if (k.key == "token") tokVal = k.value;
    CHECK_EQ(tokVal, std::string("s3cr3t"), "secret value merged back into env");
    CHECK(!fs::exists(fs::path(root) / ".secrets"), ".secrets/ deleted after migrate");
    // Idempotent: re-run does not throw, changes nothing.
    env.migrateLegacySecrets();
    tokVal.clear();
    for (auto& k : env.load("Dev").keys) if (k.key == "token") tokVal = k.value;
    CHECK_EQ(tokVal, std::string("s3cr3t"), "migrate 2nd time no-op");
}

// ---------------- Engine resolve + validate ----------------
static void test_engine(const std::string& root) {
    std::printf("[engine]\n");
    // prepare env Global + active.
    EnvironmentStore env(root);
    Environment g; g.name = "Global"; g.keys.push_back({"baseUrl", "http://global", true});
    env.save(g);
    Environment d; d.name = "Stage"; d.keys.push_back({"baseUrl", "http://stage", true});
    env.save(d);

    Engine engine(EngineConfig{root, (fs::path(root) / "appconfig.json").string()});
    engine.session().setActiveEnv("Stage");

    CHECK_EQ(engine.resolvePreview("{{baseUrl}}/x"), std::string("http://stage/x"),
             "active env overrides Global");
    engine.session().setActiveEnv("Global");
    CHECK_EQ(engine.resolvePreview("{{baseUrl}}/x"), std::string("http://global/x"),
             "fallback to Global");
    CHECK_EQ(engine.resolvePreview("{{missing}}"), std::string("{{missing}}"),
             "missing var keeps literal");

    auto vok = engine.validateJson("{\"a\": 1}");
    CHECK(vok.ok, "valid JSON");
    auto vbad = engine.validateJson("{\"a\": }");
    CHECK(!vbad.ok, "invalid JSON caught");
    CHECK(vbad.line >= 1, "has error position");

    // resolveRequest applies env vars + per-request config (timeout + TLS).
    RequestModel m; m.type = RequestType::Http; m.http.url = "{{baseUrl}}/u";
    m.config.timeoutMs = 12345; m.config.tls = false;
    auto rr = engine.resolveRequest(m);
    CHECK_EQ(rr.model.http.url, std::string("http://global/u"), "resolveRequest resolves url");
    CHECK_EQ(rr.model.http.settings.timeoutMs, 12345, "timeout from per-request config");
    CHECK(!rr.model.http.settings.verifyTls, "verifyTls from per-request config");

    // --- missingVars: flags aliases absent from the active env (Global here) ---
    RequestModel mm; mm.type = RequestType::Http;
    mm.http.url = "{{baseUrl}}/{{nope}}";
    mm.http.headers.push_back({"Authorization", "{{token}}", true});
    mm.http.headers.push_back({"X-Off", "{{disabledVar}}", false});  // disabled -> ignored
    auto miss = engine.missingVars(mm);
    CHECK_EQ(miss.size(), size_t(2), "two missing (nope, token); baseUrl resolves, disabled ignored");
    CHECK(std::find(miss.begin(), miss.end(), "nope") != miss.end(), "nope reported");
    CHECK(std::find(miss.begin(), miss.end(), "token") != miss.end(), "token reported");
    CHECK(std::find(miss.begin(), miss.end(), "baseUrl") == miss.end(), "baseUrl not missing");

    // resolves cleanly once nothing is missing
    RequestModel ok; ok.type = RequestType::Http; ok.http.url = "{{baseUrl}}/x";
    CHECK(engine.missingVars(ok).empty(), "no missing when all resolve");

    // --- aliasifyModel: literal values matching the env are rewritten back to {{alias}} ---
    // env "baseUrl" = http://global (active env is Global at this point).
    RequestModel a; a.type = RequestType::Http;
    a.http.url = "http://global/users";                       // prefix -> {{baseUrl}}
    a.http.headers.push_back({"Host", "http://global", true}); // whole -> {{baseUrl}}
    a.http.headers.push_back({"X-Lit", "literal", true});      // no match -> unchanged
    std::vector<std::string> applied;
    RequestModel ax = engine.aliasifyModel(a, &applied);
    CHECK_EQ(ax.http.url, std::string("{{baseUrl}}/users"), "url prefix aliasified");
    CHECK_EQ(ax.http.headers[0].value, std::string("{{baseUrl}}"), "header whole aliasified");
    CHECK_EQ(ax.http.headers[1].value, std::string("literal"), "non-match left unchanged");
    CHECK_EQ(applied.size(), size_t(1), "applied = {baseUrl}");

    // idempotent: re-aliasify yields no further change
    std::vector<std::string> applied2;
    RequestModel ax2 = engine.aliasifyModel(ax, &applied2);
    CHECK(applied2.empty(), "aliasify is idempotent");
    CHECK_EQ(ax2.http.url, std::string("{{baseUrl}}/users"), "url stable on second pass");

    // aliasify also covers WebSocket + GraphQL (import alias-replace) — env baseUrl=http://global active.
    RequestModel wq; wq.type = RequestType::WebSocket;
    wq.ws.url = "http://global/socket";
    wq.ws.auth.type = "bearer"; wq.ws.auth.bearerToken = "http://global";   // whole-value match
    RequestModel wqx = engine.aliasifyModel(wq);
    CHECK_EQ(wqx.ws.url, std::string("{{baseUrl}}/socket"), "ws url aliasified");
    CHECK_EQ(wqx.ws.auth.bearerToken, std::string("{{baseUrl}}"), "ws auth aliasified");

    RequestModel gq; gq.type = RequestType::GraphQL;
    gq.graphql.url = "http://global/graphql";
    gq.graphql.headers.push_back({"X-Base", "http://global", true});
    RequestModel gqx = engine.aliasifyModel(gq);
    CHECK_EQ(gqx.graphql.url, std::string("{{baseUrl}}/graphql"), "graphql url aliasified");
    CHECK_EQ(gqx.graphql.headers[0].value, std::string("{{baseUrl}}"), "graphql header aliasified");

    // env definition order decides the alias on duplicate values: "zdup" is defined BEFORE "adup"
    // (sorts later) -> the first-defined key wins, not the lexicographically smallest.
    Environment go; go.name = "Global";
    go.keys.push_back({"zdup", "http://dup.host", true});
    go.keys.push_back({"adup", "http://dup.host", true});
    env.save(go);
    engine.session().setActiveEnv("Global");
    RequestModel dupReq; dupReq.type = RequestType::Http; dupReq.http.url = "http://dup.host/p";
    std::vector<std::string> dupApplied;
    RequestModel dupOut = engine.aliasifyModel(dupReq, &dupApplied);
    CHECK_EQ(dupOut.http.url, std::string("{{zdup}}/p"), "duplicate value -> first-defined key wins");

    // --- interactionOf: routing by gRPC method type (SPEC_grpc_streaming §4) ---
    RequestModel un; un.type = RequestType::Grpc; un.grpc.methodType = "unary";
    CHECK(engine.interactionOf(un) == InteractionKind::Unary, "unary -> Unary");
    RequestModel ss; ss.type = RequestType::Grpc; ss.grpc.methodType = "server_streaming";
    CHECK(engine.interactionOf(ss) == InteractionKind::ServerStream, "server_streaming -> ServerStream");
    RequestModel cs; cs.type = RequestType::Grpc; cs.grpc.methodType = "client_streaming";
    CHECK(engine.interactionOf(cs) == InteractionKind::ClientStream, "client_streaming -> ClientStream");
    RequestModel bd; bd.type = RequestType::Grpc; bd.grpc.methodType = "bidi_streaming";
    CHECK(engine.interactionOf(bd) == InteractionKind::BiDi, "bidi_streaming -> BiDi");
    RequestModel hp; hp.type = RequestType::Http;
    CHECK(engine.interactionOf(hp) == InteractionKind::Unary, "http -> Unary");
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
    cfg.ramEffBytes = 4 * 1024;      // small RAM cap to test LRU
    cfg.diskEffBytes = 1024 * 1024;
    cfg.thresholdBytes = 1024;       // < 1KB -> prefer RAM
    cfg.enabled = true;
    cfg.persist = true;
    std::string sdir = (fs::path(root) / ".session_cache_test").string();
    fs::remove_all(sdir);
    auto cache = ResponseCache::create(cfg, sdir);

    // small (< threshold) -> RAM + write-through disk.
    cache->put("a", mkRec(100, 200));
    CHECK(cache->l1UsedBytes() > 0, "small response goes to RAM");
    CHECK(cache->l2UsedBytes() > 0, "small response write-through disk");
    auto ga = cache->get("a");
    CHECK(ga && ga->response.statusCode == 200, "get a hits L1");

    // large (>= threshold) -> disk only, NOT in RAM.
    std::uint64_t l1Before = cache->l1UsedBytes();
    cache->put("big", mkRec(4000, 200));
    CHECK_EQ(cache->l1UsedBytes(), l1Before, "large response NOT in RAM");
    auto gb = cache->get("big");
    CHECK(gb && gb->response.body.size() == 4000, "get big hits (from disk)");

    // LRU-bound RAM: stuff many small records -> does not exceed cap.
    for (int i = 0; i < 50; i++) cache->put("k" + std::to_string(i), mkRec(200, 200));
    CHECK(cache->l1UsedBytes() <= cfg.ramEffBytes, "RAM cache does not exceed cap (LRU evict)");

    // remove -> miss in both tiers.
    cache->remove("big");
    CHECK(!cache->get("big"), "remove -> miss");

    // restart: L1 empty, reload from disk; small disk-hit -> promote to L1.
    cache->put("small2", mkRec(50, 201));
    cache.reset();
    auto cache2 = ResponseCache::create(cfg, sdir);
    CHECK_EQ(cache2->l1UsedBytes(), (std::uint64_t)0, "restart: L1 empty");
    auto gs = cache2->get("small2");
    CHECK(gs && gs->response.statusCode == 201, "restart: read small2 from disk");
    CHECK(cache2->l1UsedBytes() > 0, "small disk-hit -> promote to RAM");

    // Fake driver: proves the abstraction — facade runs with any ICacheDriver, no ResponseCache changes.
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

// ENV ceiling/floor clamps user; user config is read correctly (RESPONSE_CACHE.md §1.2 + acceptance §10).
static void test_cache_config_clamp(const std::string& root) {
    std::printf("[cache_config]\n");

    // (1) Via getenv (ops): user > max -> clamp to max.
    {
        std::string cfgPath = (fs::path(root) / "appcfg_cache.json").string();
        AppConfig ac;
        ac.ramCacheSizeMb = 256;     // user > env max -> must clamp
        ac.diskCacheSizeMb = 2048;
        AppConfigStore(cfgPath).save(ac);
        // reload from file -> ensure snake_case keys round-trip + user value loaded.
        AppConfig reread = AppConfigStore(cfgPath).load();
        CHECK_EQ(reread.ramCacheSizeMb, 256, "ram_cache_size (user) round-trips through codec");
        CHECK_EQ(reread.diskCacheSizeMb, 2048, "disk_cache_size (user) round-trips through codec");
        CHECK(reread.cacheResponses && reread.cachePersist, "cache on/persist default true (not exposed to user)");

        setenv("DEED_RAM_CACHE_SIZE_MAX", "128", 1);
        setenv("DEED_DISK_CACHE_SIZE_MAX", "1024", 1);
        setenv("DEED_RAM_CACHE_THRESHOLD_KB", "256", 1);
        EngineConfig ecfg;
        ecfg.collectionRoot = (fs::path(root) / "cache_cfg_root").string();
        ecfg.appConfigPath = cfgPath;
        Engine eng(ecfg);
        const CacheConfig& cc = eng.cacheConfig();
        CHECK_EQ(cc.ramEffBytes, (std::uint64_t)128 * 1024 * 1024, "ram clamped to env max 128MB");
        CHECK_EQ(cc.diskEffBytes, (std::uint64_t)1024 * 1024 * 1024, "disk clamped to env max 1024MB");
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

    // (2) Via CacheLimits (.env loaded by UI): MIN floor raises low user; user in [min,max] unchanged.
    {
        std::string cfgPath = (fs::path(root) / "appcfg_cache2.json").string();
        AppConfig ac;
        ac.ramCacheSizeMb = 4;       // < min -> must raise to min
        ac.diskCacheSizeMb = 300;    // in [min,max] -> unchanged
        AppConfigStore(cfgPath).save(ac);

        EngineConfig ecfg;
        ecfg.collectionRoot = (fs::path(root) / "cache_cfg_root2").string();
        ecfg.appConfigPath = cfgPath;
        ecfg.cacheLimits.ramMinMb = 16; ecfg.cacheLimits.ramMaxMb = 256;
        ecfg.cacheLimits.diskMinMb = 64; ecfg.cacheLimits.diskMaxMb = 1024;
        ecfg.cacheLimits.thresholdKb = 128;
        Engine eng(ecfg);
        const CacheConfig& cc = eng.cacheConfig();
        CHECK_EQ(cc.ramEffBytes, (std::uint64_t)16 * 1024 * 1024, "ram raised to min floor 16MB");
        CHECK_EQ(cc.diskEffBytes, (std::uint64_t)300 * 1024 * 1024, "disk (user) in [min,max] keeps 300MB");
        CHECK_EQ(cc.thresholdBytes, (std::uint64_t)128 * 1024, "threshold from .env = 128KB");
    }
}

// App-config defaults from .env (EngineConfig.appDefaults) when config.json is missing a key.
static void test_app_config_defaults(const std::string& root) {
    std::printf("[app_config_defaults]\n");
    std::string cfgPath = (fs::path(root) / "appcfg_defaults.json").string();
    fs::remove(cfgPath);

    EngineConfig ec;
    ec.collectionRoot = (fs::path(root) / "defs_root").string();
    ec.appConfigPath = cfgPath;
    ec.appDefaults.fontName = "Courier";
    ec.appDefaults.fontSize = 17;
    ec.appDefaults.ramCacheSizeMb = 33;
    ec.appDefaults.diskCacheSizeMb = 77;
    ec.cacheLimits.ramMinMb = 1; ec.cacheLimits.ramMaxMb = 1000;
    ec.cacheLimits.diskMinMb = 1; ec.cacheLimits.diskMaxMb = 1000;
    Engine eng(ec);

    // No config.json yet -> load returns defaults (.env).
    AppConfig c = eng.appConfig().load();
    CHECK_EQ(c.fontName, std::string("Courier"), "font_name default from .env");
    CHECK_EQ(c.fontSize, 17, "font_size default from .env");
    CHECK_EQ(c.ramCacheSizeMb, 33, "ram_cache_size default from .env");
    CHECK_EQ(c.diskCacheSizeMb, 77, "disk_cache_size default from .env");
    CHECK_EQ(eng.cacheConfig().ramEffBytes, (std::uint64_t)33 * 1024 * 1024,
             "cache uses ram_cache_size default from .env");

    // config.json has only 1 key -> missing keys fall back to defaults (.env).
    { std::ofstream o(cfgPath); o << "{\"font_size\": 20}"; }
    AppConfigStore st(cfgPath);
    st.setDefaults(ec.appDefaults);
    AppConfig pc = st.load();
    CHECK_EQ(pc.fontSize, 20, "key present in file -> use file value");
    CHECK_EQ(pc.fontName, std::string("Courier"), "missing key -> falls back to .env default");
    CHECK_EQ(pc.ramCacheSizeMb, 33, "missing key -> ram default .env");
    fs::remove(cfgPath);
}

// ---------------- Importers ----------------
static void test_importers() {
    std::printf("[importers]\n");
    CurlImporter curl;
    CHECK(curl.canHandle("curl http://x"), "canHandle curl");
    CHECK(!curl.canHandle("wget http://x"), "does not accept wget");

    auto r = curl.parse("curl -X POST 'http://api.test/users?q=1' "
                        "-H 'Content-Type: application/json' "
                        "-H 'Authorization: Bearer abc' "
                        "-d '{\"name\":\"Alice\"}'");
    CHECK(r.ok, "parse curl ok");
    CHECK_EQ(r.model.http.method, std::string("POST"), "method POST");
    // Importer splits the query string out of the URL into params (url_util::splitUrlQuery).
    CHECK_EQ(r.model.http.url, std::string("http://api.test/users"), "url with query stripped");
    CHECK_EQ(r.model.http.params.size(), size_t(1), "query split into 1 param");
    CHECK_EQ(r.model.http.params[0].key, std::string("q"), "param key q");
    CHECK_EQ(r.model.http.params[0].value, std::string("1"), "param value 1");
    CHECK_EQ(r.model.http.body.mode, std::string("json"), "body json from content-type");
    CHECK_EQ(r.model.http.body.json, std::string("{\"name\":\"Alice\"}"), "body content");
    CHECK_EQ(r.model.http.headers.size(), size_t(1), "1 header (Authorization -> Auth)");
    CHECK_EQ(r.model.http.auth.type, std::string("bearer"), "Authorization Bearer -> bearer auth");
    CHECK_EQ(r.model.http.auth.bearerToken, std::string("abc"), "bearer token into Auth tab");

    auto rb = curl.parse("curl -u user:pass http://api.test/secure");
    CHECK_EQ(rb.model.http.auth.type, std::string("basic"), "-u -> basic auth");
    CHECK_EQ(rb.model.http.auth.basicUsername, std::string("user"), "basic user");
    CHECK_EQ(rb.model.http.method, std::string("GET"), "no body -> GET");

    GrpcImporter g;
    CHECK(g.canHandle("grpcurl -plaintext localhost:50051 pkg.Svc/M"), "canHandle grpcurl");
    auto gr = g.parse("grpcurl -plaintext -d '{\"id\":\"1\"}' -H 'authorization: Bearer t' "
                      "localhost:50051 user.v1.UserService/GetUser");
    CHECK(gr.ok, "parse grpcurl ok");
    CHECK_EQ(gr.model.grpc.target, std::string("localhost:50051"), "target");
    // Import intentionally SKIPS the RPC (Service/Method) — only target/message/metadata/tls are imported.
    CHECK(gr.model.grpc.service.empty(), "service skipped on import");
    CHECK(gr.model.grpc.method.empty(), "method skipped on import");
    CHECK_EQ(gr.model.grpc.tls.enabled, false, "-plaintext -> tls off");
    CHECK_EQ(gr.model.grpc.metadata.size(), size_t(1), "1 metadata");

    auto gr2 = g.parse("grpcs://localhost:50051/pkg.Service/Method");
    CHECK(gr2.ok, "parse compact string ok");
    CHECK_EQ(gr2.model.grpc.tls.enabled, true, "grpcs -> tls on");
    CHECK_EQ(gr2.model.grpc.target, std::string("localhost:50051"), "target from compact string");
    CHECK(gr2.model.grpc.service.empty(), "compact: service skipped on import");
}

static void test_field_codec() {
    std::printf("[field_codec]\n");
    std::string body = "{\"a\":1}";
    CHECK(fieldcodec::formatJson(body, true).find('\n') != std::string::npos, "pretty has newlines");
    CHECK_EQ(fieldcodec::formatJson(body, false), std::string("{\"a\":1}"), "compact drops whitespace");
    std::string enc = fieldcodec::jsonEncodeString(body);
    CHECK(enc.front() == '"' && enc.back() == '"', "encode -> string literal has quotes");
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
    CHECK(c.find("curl -X POST") != std::string::npos, "has method");
    CHECK(c.find("api.test/users") != std::string::npos, "has url");
    CHECK(c.find("--data") != std::string::npos, "has body");
    CHECK(c.find("X-Token: abc123") != std::string::npos, "has X-Token header");
    CHECK(c.find("q=hello") != std::string::npos, "has param q");

    GrpcRequest& g = m.grpc;
    m.type = RequestType::Grpc;
    g.target = "localhost:50051"; g.service = "pkg.Svc"; g.method = "M"; g.message = "{\"id\":\"1\"}";
    std::string gc = toCurl(m);
    CHECK(gc.find("grpcurl") != std::string::npos, "grpc -> grpcurl");
    CHECK(gc.find("pkg.Svc/M") != std::string::npos, "has service/method");
}

// Regression tests for the audit remediation (AUDIT_REMEDIATION_SPEC.md).
static void test_audit_fixes() {
    std::printf("[audit_fixes]\n");

    // H5: pathologically deep JSON is rejected by the depth guard (returns false, does NOT crash).
    {
        std::string deep(500, '[');   // 500 levels, well past kMaxJsonDepth
        std::vector<KeyValue> kv; std::string err;
        CHECK(!fieldcodec::jsonToKeyValues(deep, kv, err), "H5: deep JSON rejected, no stack overflow");
        CHECK(fieldcodec::jsonToKeyValues("[]", kv, err), "H5: shallow JSON still parses");
    }

    CurlImporter curl;
    // M15: valid Basic decodes; malformed Basic is NOT silently accepted as credentials.
    {
        auto good = curl.parse("curl -H 'Authorization: Basic dXNlcjpwYXNz' http://x.test");  // user:pass
        CHECK(good.ok && good.model.http.auth.type == "basic" &&
              good.model.http.auth.basicUsername == "user", "M15: valid Basic -> basic creds");
        auto bad = curl.parse("curl -H 'Authorization: Basic not_base64!!' http://x.test");
        CHECK(bad.ok && bad.model.http.auth.type == "apikey",
              "M15: malformed Basic -> apikey (not garbled creds)");
    }
    // M14: an empty inline value (--data=) must not swallow the next token as data.
    {
        auto d = curl.parse("curl --data= http://x.test");
        CHECK(d.ok && d.model.http.url.find("x.test") != std::string::npos,
              "M14: empty --data= keeps the URL");
    }

    // Per-request config: grpc import carries TLS intent into RequestConfig.tls.
    {
        GrpcImporter g;
        auto plain = g.parse("grpcurl -plaintext localhost:50051 pkg.Svc/M");
        CHECK(plain.ok && plain.model.config.tls == false, "config.tls follows -plaintext (off)");
        auto secure = g.parse("grpcs://localhost:50051/pkg.Svc/M");
        CHECK(secure.ok && secure.model.config.tls == true, "config.tls follows grpcs:// (on)");
    }
}

int main() {
    std::string root = makeTempRoot();
    std::printf("Temp root: %s\n", root.c_str());

    test_variable_resolver();
    test_alias_inversion();
    test_field_codec();
    test_curl_export();
    test_request_naming();
    test_filename_migration(root);
    test_response_cache(root);
    test_cache_config_clamp(root);
    test_app_config_defaults(root);
    test_collection_store(root);
    test_session_store(root);
    test_env_and_secret(root);
    test_secret_migration(root);
    test_engine(root);
    test_importers();
    test_audit_fixes();

    int streamFail = run_stream_sink_tests();   // INV-1 gatekeeper (transport-free)
    int wsFail = run_ws_session_tests();        // INV-1 duplex gatekeeper (transport-free)
    int sseFail = run_sse_parser_tests();       // SSE wire parser gatekeeper (transport-free)
    int gqlFail = run_gql_ws_protocol_tests();  // GraphQL-over-WS protocol gatekeeper (transport-free)

    fs::remove_all(root);

    std::printf("\n==== %d passed, %d failed (+%d stream, +%d ws, +%d sse, +%d gql failures) ====\n",
                g_pass, g_fail, streamFail, wsFail, sseFail, gqlFail);
    return (g_fail == 0 && streamFail == 0 && wsFail == 0 && sseFail == 0 && gqlFail == 0) ? 0 : 1;
}
