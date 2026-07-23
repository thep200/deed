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

#include "core/infra/cache/cache.hpp"
#include "core/infra/export/exporter.hpp" // toCurl (export)
#include "core/infra/import/importer.hpp"
#include "core/infra/persistence/request_naming.hpp"
#include "core/infra/persistence/stores.hpp"
#include "core/infra/serialization/field_json.hpp"
#include "core/infra/variables/variable_resolver.hpp"
#include "core/app/core_api_client.hpp" // domain stack facade (replaces Engine in these tests)
#include "app/cache_config.hpp"         // detail::buildCacheConfig (core/src; white-box include path)

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
// Defined in mapper_roundtrip_test.cpp — REFACTOR_SPEC §8.1 JSON<->domain round-trip gate. Returns #failures.
int run_mapper_roundtrip_tests();
// Defined in saga_test.cpp — REFACTOR_SPEC §11.3 saga/orchestrator gate (fakes). Returns #failures.
int run_saga_tests();
// Defined in repository_test.cpp — REFACTOR_SPEC §8.3 ICollectionRepository (domain objects). Returns #failures.
int run_repository_tests();
// Defined in import_service_test.cpp — REFACTOR_SPEC P6 IImportService (curl/grpcurl/graphql -> domain). #failures.
int run_import_service_tests();
// Defined in persistence_repo_test.cpp — REFACTOR_SPEC §6.3 env/session/appConfig repository ports. #failures.
int run_persistence_repo_tests();
int run_field_json_tests();

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

// ---------------- Multi-mode HTTP body persistence ----------------
// REFACTOR_SPEC D-step2: the store now serializes requests through the NATIVE domain mapper, and the domain
// Body is a single-variant (only the ACTIVE mode persists). The legacy multi-mode-at-once behavior is gone
// by design — saving in "json" mode keeps the json content and drops any inactive text/xml/form content.
// Domain-store test helpers (CollectionStore speaks domain RequestModel now).
namespace ts {
namespace d = core::domain;
const d::HttpRequest& http(const d::RequestModel& m) { return std::get<d::HttpRequest>(m.payload()); }
const d::WebSocketRequest& ws(const d::RequestModel& m) { return std::get<d::WebSocketRequest>(m.payload()); }
const d::GraphQlRequest& gql(const d::RequestModel& m) { return std::get<d::GraphQlRequest>(m.payload()); }
std::string bearerOf(const d::Auth& a) {
    std::string t;
    a.match([&](auto&& x) { using T = std::decay_t<decltype(x)>; if constexpr (std::is_same_v<T, d::AuthBearer>) t = x.token; });
    return t;
}
std::string bodyMode(const d::Body& b) {
    std::string mode = "none";
    b.match([&](auto&& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, d::BodyRaw>)
            mode = x.subtype == d::RawSubtype::Json ? "json" : x.subtype == d::RawSubtype::Xml ? "xml" : "text";
        else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) mode = "form-urlencoded";
        else if constexpr (std::is_same_v<T, d::BodyMultipart>) mode = "multipart";
        else if constexpr (std::is_same_v<T, d::BodyBinary>) mode = "binary";
    });
    return mode;
}
std::string rawText(const d::Body& b) {
    std::string t;
    b.match([&](auto&& x) { using T = std::decay_t<decltype(x)>; if constexpr (std::is_same_v<T, d::BodyRaw>) t = x.text; });
    return t;
}
// Rebuild the HTTP payload with a new method/url/body (keeps id/name/seq/config + pathVars/params/headers/auth).
d::RequestModel withHttp(const d::RequestModel& m, d::HttpMethod method, const std::string& url, d::Body body) {
    const auto& h = http(m);
    d::HttpRequest::Parts p{method, d::Url::create(url).take(), h.pathVariables(), h.params(), h.headers(),
                            std::move(body), h.auth()};
    return d::RequestModel::create(m.id(), m.name(), m.seq(), m.config(), d::HttpRequest::create(std::move(p)).take())
        .take();
}
} // namespace ts

static void test_body_singlemode_roundtrip(const std::string& root) {
    std::printf("[body_singlemode]\n");
    CollectionStore store(root);
    std::string rel = store.createRequest("", RequestType::Http, "Body Mode");

    auto m = store.loadRequest(rel);
    m = ts::withHttp(m, core::domain::HttpMethod::Get, "",
                     core::domain::Body::raw(core::domain::RawSubtype::Json, "{\"k\":1}"));
    rel = store.saveRequest(rel, m);

    auto r = store.loadRequest(rel);
    CHECK_EQ(ts::bodyMode(ts::http(r).body()), std::string("json"), "active body mode round-trip");
    CHECK_EQ(ts::rawText(ts::http(r).body()), std::string("{\"k\":1}"), "active json kept");

    // form-urlencoded as the ACTIVE mode round-trips its entries (key/value/enabled).
    std::vector<core::domain::FormField> ff{{"f1", "v1", true}, {"f2", "v2", false}};
    auto f = ts::withHttp(store.loadRequest(rel), core::domain::HttpMethod::Get, "",
                          core::domain::Body::formUrlEncoded(ff));
    rel = store.saveRequest(rel, f);
    auto fr = store.loadRequest(rel);
    CHECK_EQ(ts::bodyMode(ts::http(fr).body()), std::string("form-urlencoded"), "active form mode round-trip");

    // UI body drafts: the user filled BOTH json and form; active=json persists in the domain Body, while the
    // form draft rides along in "_uiBodyDrafts" so switching the editor's body mode after reload isn't empty.
    auto jm = ts::withHttp(store.loadRequest(rel), core::domain::HttpMethod::Get, "",
                           core::domain::Body::raw(core::domain::RawSubtype::Json, "{\"a\":1}"));
    std::map<std::string, std::string> drafts{{"json", "{\"a\":1}"},
                                              {"form-urlencoded", "[{\"key\":\"x\",\"value\":\"y\",\"enabled\":1}]"}};
    rel = store.saveRequest(rel, jm, drafts);
    auto back = store.loadBodyDrafts(rel);
    CHECK_EQ(back["form-urlencoded"], std::string("[{\"key\":\"x\",\"value\":\"y\",\"enabled\":1}]"),
             "non-active form draft persisted");
    CHECK_EQ(back["json"], std::string("{\"a\":1}"), "active json draft persisted");
    CHECK_EQ(ts::bodyMode(ts::http(store.loadRequest(rel)).body()), std::string("json"),
             "domain Body still single-mode (json active) — drafts don't affect the sent body");
    // 2-arg save (non-editor caller, e.g. rename/autosave w/o drafts) PRESERVES existing drafts.
    rel = store.saveRequest(rel, store.loadRequest(rel));
    CHECK_EQ(store.loadBodyDrafts(rel)["form-urlencoded"],
             std::string("[{\"key\":\"x\",\"value\":\"y\",\"enabled\":1}]"), "drafts preserved by 2-arg save");
}

// ---------------- CollectionStore round-trip + CRUD ----------------
static void test_collection_store(const std::string& root) {
    std::printf("[collection_store]\n");
    CollectionStore store(root);

    std::string rel = store.createRequest("", RequestType::Http, "Get Users");
    CHECK(!rel.empty(), "create request returns relPath");
    CHECK(fs::exists(fs::path(root) / rel), "request file exists");

    auto m = store.loadRequest(rel);
    CHECK_EQ(m.name(), std::string("Get Users"), "name preserved");
    CHECK(m.type() == core::domain::RequestType::Http, "type = http");
    CHECK(!m.id().get().empty(), "has id");
    // unique id + find by id (fixes bug deleting wrong file due to duplicate id/path).
    std::string r2 = store.createRequest("", RequestType::Http, "Another");
    auto ma = store.loadRequest(r2);
    CHECK(!ma.id().get().empty() && ma.id() != m.id(), "2 requests have different ids");
    CHECK_EQ(store.findRelPathById(m.id().get()), rel, "findRelPathById returns correct path");
    CHECK(store.findRelPathById("req_nope").empty(), "missing id -> empty");
    store.remove(r2);

    CHECK(ts::http(m).headers().size() >= 5, "new HTTP request has default headers");
    bool hasContentType = false;
    for (const auto& h : ts::http(m).headers().items()) if (h.name() == "Content-Type") hasContentType = true;
    CHECK(hasContentType, "has default Content-Type");
    // New requests auto-identify with an ENABLED User-Agent="deed"; the other hint headers stay off.
    bool uaOk = false, ctEnabled = false;
    for (const auto& h : ts::http(m).headers().items()) {
        if (h.name() == "User-Agent") uaOk = (h.value() == "deed" && h.enabled());
        if (h.name() == "Content-Type") ctEnabled = h.enabled();
    }
    CHECK(uaOk, "User-Agent=deed enabled on new request");
    CHECK(!ctEnabled, "other hint headers remain disabled");

    // edit + save + reload (round-trip). Change method -> filename must change http_get_* -> http_post_*.
    m = ts::withHttp(m, core::domain::HttpMethod::Post, "{{baseUrl}}/users",
                     core::domain::Body::raw(core::domain::RawSubtype::Json, "{\"a\":1}"));
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
    auto m2 = store.loadRequest(rel);
    CHECK_EQ(core::domain::toString(ts::http(m2).method()), std::string("POST"), "method round-trip");
    CHECK_EQ(ts::http(m2).url().raw(), std::string("{{baseUrl}}/users"), "url round-trip");
    CHECK_EQ(ts::rawText(ts::http(m2).body()), std::string("{\"a\":1}"), "body json round-trip");

    // folder + nested request.
    std::string folder = store.createFolder("", "Folder A");
    CHECK(fs::is_directory(fs::path(root) / folder), "folder created");
    std::string nested = store.createRequest(folder, RequestType::Grpc, "Get User");
    auto gm = store.loadRequest(nested);
    CHECK(gm.type() == core::domain::RequestType::Grpc, "nested = grpc");

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
        CHECK_EQ(s.getActiveEnv(), std::string(""), "active env defaults to none (no special base)");
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
    g.keys.push_back({"wsBase", "ws://global", true}); // ws-scheme prefix (domain ws urls must be ws/wss)
    env.save(g);
    Environment d; d.name = "Stage"; d.keys.push_back({"baseUrl", "http://stage", true});
    env.save(d);

    // Drive the domain stack directly (no Engine): CoreApiClient owns its stores + exposes the same
    // resolve/validate/aliasify/interactionOf facade natively. (missingVars + aliasify `applied` out-param
    // were Engine-only and unused by the live path -> dropped; validateJson now returns Status.)
    auto client = core::app::CoreApiClient::create(
        core::app::CoreApiClient::Config{root, (fs::path(root) / "appconfig.json").string()});
    client->session().setActiveEnv("Stage");

    CHECK_EQ(client->resolvePreview("{{baseUrl}}/x"), std::string("http://stage/x"), "active env Stage");
    client->session().setActiveEnv("Global");
    CHECK_EQ(client->resolvePreview("{{baseUrl}}/x"), std::string("http://global/x"), "active env Global");
    CHECK_EQ(client->resolvePreview("{{missing}}"), std::string("{{missing}}"), "missing var keeps literal");

    CHECK(client->validateJson(core::domain::JsonText::of("{\"a\": 1}")).isOk(), "valid JSON");
    CHECK(!client->validateJson(core::domain::JsonText::of("{\"a\": }")).isOk(), "invalid JSON caught");

    // aliasifyModel / interactionOf / exportCurl are domain-typed; build domain fixtures directly (no bridge).
    namespace d2 = core::domain;
    const d2::RequestConfig cfg{d2::Timeout::fromMillis(1800000).take(), true};
    auto hdr = [](const std::string &k, const std::string &v) { return d2::Header::create(k, v).take(); };
    auto mkHttp = [&](const std::string &url, std::vector<d2::Header> hdrs) {
      d2::HttpRequest::Parts hp{d2::HttpMethod::Get, d2::Url::create(url).take(), d2::PathVariableList{},
                                d2::QueryParamList{}, d2::HeaderList{std::move(hdrs)}, d2::Body::none(),
                                d2::Auth::none()};
      return d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                      d2::HttpRequest::create(std::move(hp)).take())
          .take();
    };
    auto aliasify = [&](const d2::RequestModel &mm) { return client->aliasifyModel(mm); };

    // exportCurl resolves {{vars}} + per-request config, then renders the cURL command.
    std::string curl = client->exportCurl(mkHttp("{{baseUrl}}/u", {}));
    CHECK(curl.find("http://global/u") != std::string::npos, "exportCurl resolves url");

    // --- aliasifyModel: literal values matching the env are rewritten back to {{alias}} ---
    // env "baseUrl" = http://global (active env is Global at this point).
    d2::RequestModel ax =
        aliasify(mkHttp("http://global/users", {hdr("Host", "http://global"), hdr("X-Lit", "literal")}));
    CHECK_EQ(ts::http(ax).url().raw(), std::string("{{baseUrl}}/users"), "url prefix aliasified");
    CHECK_EQ(ts::http(ax).headers().items()[0].value(), std::string("{{baseUrl}}"), "header whole aliasified");
    CHECK_EQ(ts::http(ax).headers().items()[1].value(), std::string("literal"), "non-match left unchanged");

    // idempotent: re-aliasify yields no further change
    d2::RequestModel ax2 = aliasify(ax);
    CHECK_EQ(ts::http(ax2).url().raw(), std::string("{{baseUrl}}/users"), "url stable on second pass");

    // aliasify also covers WebSocket + GraphQL (import alias-replace) — env baseUrl=http://global active.
    d2::WebSocketRequest::Parts wqp{d2::Url::create("ws://global/socket").take()}; // ws url must be ws/wss
    wqp.auth = d2::Auth::bearer("http://global").take();                            // whole-value match
    d2::RequestModel wqx = aliasify(
        d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                 d2::WebSocketRequest::create(std::move(wqp)).take())
            .take());
    CHECK_EQ(ts::ws(wqx).url().raw(), std::string("{{wsBase}}/socket"), "ws url aliasified");
    CHECK_EQ(ts::bearerOf(ts::ws(wqx).auth()), std::string("{{baseUrl}}"), "ws auth aliasified");

    d2::GraphQlOperation gop;
    gop.query = "query { me }"; // domain graphql needs a query
    d2::GraphQlRequest::Parts gqp{d2::Url::create("http://global/graphql").take(), gop,
                                  d2::HeaderList{std::vector<d2::Header>{hdr("X-Base", "http://global")}},
                                  d2::Auth::none(), d2::GqlSubTransport::Http, ""};
    d2::RequestModel gqx = aliasify(
        d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                 d2::GraphQlRequest::create(std::move(gqp)).take())
            .take());
    CHECK_EQ(ts::gql(gqx).url().raw(), std::string("{{baseUrl}}/graphql"), "graphql url aliasified");
    CHECK_EQ(ts::gql(gqx).headers().items()[0].value(), std::string("{{baseUrl}}"), "graphql header aliasified");

    // env definition order decides the alias on duplicate values: "zdup" is defined BEFORE "adup"
    // (sorts later) -> the first-defined key wins, not the lexicographically smallest.
    Environment go; go.name = "Global";
    go.keys.push_back({"zdup", "http://dup.host", true});
    go.keys.push_back({"adup", "http://dup.host", true});
    env.save(go);
    client->session().setActiveEnv("Global");
    d2::RequestModel dupOut = aliasify(mkHttp("http://dup.host/p", {}));
    CHECK_EQ(ts::http(dupOut).url().raw(), std::string("{{zdup}}/p"), "duplicate value -> first-defined key wins");

    // --- interactionOf: routing by gRPC method type (SPEC_grpc_streaming §4) ---
    auto mkGrpc = [&](d2::GrpcMethodType mt) {
      d2::GrpcRequest::Parts gp;
      gp.methodType = mt;
      return d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                      d2::GrpcRequest::create(std::move(gp)).take())
          .take();
    };
    auto io = [&](const d2::RequestModel &mm) { return client->interactionOf(mm); };
    CHECK(io(mkGrpc(d2::GrpcMethodType::Unary)) == InteractionKind::Unary, "unary -> Unary");
    CHECK(io(mkGrpc(d2::GrpcMethodType::ServerStreaming)) == InteractionKind::ServerStream,
          "server_streaming -> ServerStream");
    CHECK(io(mkGrpc(d2::GrpcMethodType::ClientStreaming)) == InteractionKind::ClientStream,
          "client_streaming -> ClientStream");
    CHECK(io(mkGrpc(d2::GrpcMethodType::BidiStreaming)) == InteractionKind::BiDi, "bidi_streaming -> BiDi");
    CHECK(io(mkHttp("http://x", {})) == InteractionKind::Unary, "http -> Unary");
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

// Durability + filename fixes: index write-through (Fix 1) and bare-id filename + orphan sweep (Fix 3).
static void test_cache_durability(const std::string& root) {
    std::printf("[cache_durability]\n");
    CacheConfig cfg;
    cfg.ramEffBytes = 1024 * 1024;
    cfg.diskEffBytes = 1024 * 1024;
    cfg.thresholdBytes = 1024;
    cfg.enabled = true; cfg.persist = true;

    // Fix 1: a put persists _index.json IMMEDIATELY (not batched), so a fresh view of the same dir sees it
    // WITHOUT the first cache being destroyed — mimics app terminate that never runs dtors (the stream bug).
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
        // Effective config is built by the shared detail::buildCacheConfig (env-var ceilings clamp the user
        // level) — the same builder CoreApiClient's native cache uses. (put/get round-trip is covered by the
        // direct ResponseCache tests above.)
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

// App-config defaults from .env (EngineConfig.appDefaults) when config.json is missing a key.
static void test_app_config_defaults(const std::string& root) {
    std::printf("[app_config_defaults]\n");
    std::string cfgPath = (fs::path(root) / "appcfg_defaults.json").string();
    fs::remove(cfgPath);

    CHECK_EQ(AppConfig{}.theme, std::string("light"), "built-in theme default is light");

    AppConfig defaults;
    defaults.fontName = "Courier";
    defaults.fontSize = 17;
    defaults.theme = "dark";
    defaults.ramCacheSizeMb = 33;
    defaults.diskCacheSizeMb = 77;
    core::CacheLimits lim;
    lim.ramMinMb = 1; lim.ramMaxMb = 1000;
    lim.diskMinMb = 1; lim.diskMaxMb = 1000;

    // No config.json yet -> load returns defaults (.env). (AppConfigStore directly — no Engine.)
    AppConfigStore store(cfgPath);
    store.setDefaults(defaults);
    AppConfig c = store.load();
    CHECK_EQ(c.fontName, std::string("Courier"), "font_name default from .env");
    CHECK_EQ(c.fontSize, 17, "font_size default from .env");
    CHECK_EQ(c.theme, std::string("dark"), "theme default from .env");
    CHECK_EQ(c.ramCacheSizeMb, 33, "ram_cache_size default from .env");
    CHECK_EQ(c.diskCacheSizeMb, 77, "disk_cache_size default from .env");
    CHECK_EQ(core::detail::buildCacheConfig(c, lim).ramEffBytes, (std::uint64_t)33 * 1024 * 1024,
             "cache uses ram_cache_size default from .env");

    // config.json has only 1 key -> missing keys fall back to defaults (.env).
    { std::ofstream o(cfgPath); o << "{\"font_size\": 20}"; }
    AppConfigStore st(cfgPath);
    st.setDefaults(defaults);
    AppConfig pc = st.load();
    CHECK_EQ(pc.fontSize, 20, "key present in file -> use file value");
    CHECK_EQ(pc.fontName, std::string("Courier"), "missing key -> falls back to .env default");
    CHECK_EQ(pc.theme, std::string("dark"), "missing theme key -> falls back to .env default");
    CHECK_EQ(pc.ramCacheSizeMb, 33, "missing key -> ram default .env");

    // theme present in the file overrides the default; save/load round-trips it.
    { std::ofstream o(cfgPath); o << "{\"theme\": \"light\"}"; }
    CHECK_EQ(st.load().theme, std::string("light"), "theme from file overrides .env default");
    AppConfig rt = st.load();
    rt.theme = "dark";
    st.save(rt);
    CHECK_EQ(st.load().theme, std::string("dark"), "theme survives save/load round-trip");
    fs::remove(cfgPath);
}

// ---------------- Importers (domain output) ----------------
namespace impv { // small views over the domain payload so the importer checks stay readable
namespace d = core::domain;
const d::HttpRequest& httpOf(const core::ImportParseResult& r) {
    return std::get<d::HttpRequest>(r.model->payload());
}
const d::GrpcRequest& grpcOf(const core::ImportParseResult& r) {
    return std::get<d::GrpcRequest>(r.model->payload());
}
struct BodyView { std::string mode, content; };
BodyView bodyView(const d::Body& b) {
    BodyView v{"none", ""};
    b.match([&](auto&& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, d::BodyRaw>) {
            v.mode = x.subtype == d::RawSubtype::Json ? "json" : x.subtype == d::RawSubtype::Xml ? "xml" : "text";
            v.content = x.text;
        } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) v.mode = "form-urlencoded";
        else if constexpr (std::is_same_v<T, d::BodyMultipart>) v.mode = "multipart";
        else if constexpr (std::is_same_v<T, d::BodyBinary>) v.mode = "binary";
    });
    return v;
}
struct AuthView { std::string type, bearer, basicUser; };
AuthView authView(const d::Auth& a) {
    AuthView v{"none", "", ""};
    a.match([&](auto&& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, d::AuthBearer>) { v.type = "bearer"; v.bearer = x.token; }
        else if constexpr (std::is_same_v<T, d::AuthBasic>) { v.type = "basic"; v.basicUser = x.username; }
        else if constexpr (std::is_same_v<T, d::AuthApiKey>) v.type = "apikey";
    });
    return v;
}
} // namespace impv

static void test_importers() {
    std::printf("[importers]\n");
    CurlImporter curl;
    CHECK(curl.canHandle("curl http://x"), "canHandle curl");
    CHECK(!curl.canHandle("wget http://x"), "does not accept wget");

    auto r = curl.parse("curl -X POST 'http://api.test/users?q=1' "
                        "-H 'Content-Type: application/json' "
                        "-H 'Authorization: Bearer abc' "
                        "-d '{\"name\":\"Alice\"}'");
    CHECK(r.ok && r.model, "parse curl ok");
    const auto& h = impv::httpOf(r);
    CHECK_EQ(core::domain::toString(h.method()), std::string("POST"), "method POST");
    // Importer splits the query string out of the URL into params (url_util::splitUrlQuery).
    CHECK_EQ(h.url().raw(), std::string("http://api.test/users"), "url with query stripped");
    CHECK_EQ(h.params().size(), size_t(1), "query split into 1 param");
    CHECK_EQ(h.params().items()[0].key(), std::string("q"), "param key q");
    CHECK_EQ(h.params().items()[0].value(), std::string("1"), "param value 1");
    CHECK_EQ(impv::bodyView(h.body()).mode, std::string("json"), "body json from content-type");
    CHECK_EQ(impv::bodyView(h.body()).content, std::string("{\"name\":\"Alice\"}"), "body content");
    CHECK_EQ(h.headers().size(), size_t(1), "1 header (Authorization -> Auth)");
    CHECK_EQ(impv::authView(h.auth()).type, std::string("bearer"), "Authorization Bearer -> bearer auth");
    CHECK_EQ(impv::authView(h.auth()).bearer, std::string("abc"), "bearer token into Auth tab");

    auto rb = curl.parse("curl -u user:pass http://api.test/secure");
    CHECK_EQ(impv::authView(impv::httpOf(rb).auth()).type, std::string("basic"), "-u -> basic auth");
    CHECK_EQ(impv::authView(impv::httpOf(rb).auth()).basicUser, std::string("user"), "basic user");
    CHECK_EQ(core::domain::toString(impv::httpOf(rb).method()), std::string("GET"), "no body -> GET");

    GrpcImporter g;
    CHECK(g.canHandle("grpcurl -plaintext localhost:50051 pkg.Svc/M"), "canHandle grpcurl");
    auto gr = g.parse("grpcurl -plaintext -d '{\"id\":\"1\"}' -H 'authorization: Bearer t' "
                      "localhost:50051 user.v1.UserService/GetUser");
    CHECK(gr.ok && gr.model, "parse grpcurl ok");
    CHECK_EQ(impv::grpcOf(gr).target(), std::string("localhost:50051"), "target");
    // Import intentionally SKIPS the RPC (Service/Method) — only target/message/metadata/tls are imported.
    CHECK(impv::grpcOf(gr).service().empty(), "service skipped on import");
    CHECK(impv::grpcOf(gr).method().empty(), "method skipped on import");
    CHECK_EQ(impv::grpcOf(gr).tls().enabled(), false, "-plaintext -> tls off");
    CHECK_EQ(impv::grpcOf(gr).metadata().entries().size(), size_t(1), "1 metadata");

    auto gr2 = g.parse("grpcs://localhost:50051/pkg.Service/Method");
    CHECK(gr2.ok && gr2.model, "parse compact string ok");
    CHECK_EQ(impv::grpcOf(gr2).tls().enabled(), true, "grpcs -> tls on");
    CHECK_EQ(impv::grpcOf(gr2).target(), std::string("localhost:50051"), "target from compact string");
    CHECK(impv::grpcOf(gr2).service().empty(), "compact: service skipped on import");
}

static void test_curl_export() {
    std::printf("[curl_export]\n");
    namespace d = core::domain;
    const d::RequestConfig cfg{d::Timeout::fromMillis(30000).take(), true};

    // HTTP: POST with a JSON body, a custom header, and a query param.
    std::vector<d::Header> hs;
    hs.push_back(d::Header::create("Content-Type", "application/json").take());
    hs.push_back(d::Header::create("X-Token", "abc123").take());
    std::vector<d::QueryParam> qp;
    qp.push_back(d::QueryParam::create("q", "hello").take());
    d::HttpRequest::Parts hp{d::HttpMethod::Post, d::Url::create("https://api.test/users").take(),
                             d::PathVariableList{}, d::QueryParamList{std::move(qp)},
                             d::HeaderList{std::move(hs)}, d::Body::raw(d::RawSubtype::Json, "{\"a\":1}"),
                             d::Auth::none()};
    auto httpModel = d::RequestModel::create(d::RequestId(""), "curl-http", 0, cfg,
                                             d::HttpRequest::create(std::move(hp)).take())
                         .take();
    std::string c = toCurl(httpModel);
    CHECK(c.find("curl -X POST") != std::string::npos, "has method");
    CHECK(c.find("api.test/users") != std::string::npos, "has url");
    CHECK(c.find("--data") != std::string::npos, "has body");
    CHECK(c.find("X-Token: abc123") != std::string::npos, "has X-Token header");
    CHECK(c.find("q=hello") != std::string::npos, "has param q");

    // gRPC: grpcurl form (tls off by config -> -plaintext).
    d::GrpcRequest::Parts gp;
    gp.target = "localhost:50051"; gp.service = "pkg.Svc"; gp.method = "M";
    gp.message = d::JsonText::of("{\"id\":\"1\"}");
    auto grpcModel = d::RequestModel::create(d::RequestId(""), "curl-grpc", 0,
                                             d::RequestConfig{d::Timeout::fromMillis(30000).take(), false},
                                             d::GrpcRequest::create(std::move(gp)).take())
                         .take();
    std::string gc = toCurl(grpcModel);
    CHECK(gc.find("grpcurl") != std::string::npos, "grpc -> grpcurl");
    CHECK(gc.find("-plaintext") != std::string::npos, "grpc tls off -> -plaintext");
    CHECK(gc.find("pkg.Svc/M") != std::string::npos, "has service/method");
}

// Regression tests for the audit remediation (AUDIT_REMEDIATION_SPEC.md).
static void test_audit_fixes() {
    std::printf("[audit_fixes]\n");

    // H5: pathologically deep JSON is rejected by the depth guard (returns false, does NOT crash).
    {
        std::string deep(500, '[');   // 500 levels, well past kMaxJsonDepth
        CHECK(!core::serial::jsonToHeaders(deep).isOk(), "H5: deep JSON rejected, no stack overflow");
        CHECK(core::serial::jsonToHeaders("[]").isOk(), "H5: shallow JSON still parses");
    }

    CurlImporter curl;
    // M15: valid Basic decodes; malformed Basic is NOT silently accepted as credentials.
    {
        auto good = curl.parse("curl -H 'Authorization: Basic dXNlcjpwYXNz' http://x.test");  // user:pass
        auto gv = impv::authView(impv::httpOf(good).auth());
        CHECK(good.ok && gv.type == "basic" && gv.basicUser == "user", "M15: valid Basic -> basic creds");
        auto bad = curl.parse("curl -H 'Authorization: Basic not_base64!!' http://x.test");
        CHECK(bad.ok && impv::authView(impv::httpOf(bad).auth()).type == "apikey",
              "M15: malformed Basic -> apikey (not garbled creds)");
    }
    // M14: an empty inline value (--data=) must not swallow the next token as data.
    {
        auto dd = curl.parse("curl --data= http://x.test");
        CHECK(dd.ok && impv::httpOf(dd).url().raw().find("x.test") != std::string::npos,
              "M14: empty --data= keeps the URL");
    }

    // Per-request config: grpc import carries TLS intent into RequestConfig.tls.
    {
        GrpcImporter g;
        auto plain = g.parse("grpcurl -plaintext localhost:50051 pkg.Svc/M");
        CHECK(plain.ok && plain.model->config().tlsEnabledDefault == false, "config.tls follows -plaintext (off)");
        auto secure = g.parse("grpcs://localhost:50051/pkg.Svc/M");
        CHECK(secure.ok && secure.model->config().tlsEnabledDefault == true, "config.tls follows grpcs:// (on)");
    }
}

int main() {
    std::string root = makeTempRoot();
    std::printf("Temp root: %s\n", root.c_str());

    test_variable_resolver();
    test_alias_inversion();
    test_curl_export();
    test_request_naming();
    test_filename_migration(root);
    test_response_cache(root);
    test_cache_durability(root);
    test_cache_config_clamp(root);
    test_app_config_defaults(root);
    test_collection_store(root);
    test_body_singlemode_roundtrip(root);
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
    int mapFail = run_mapper_roundtrip_tests(); // REFACTOR_SPEC §8.1 JSON<->domain round-trip gate
    int sagaFail = run_saga_tests();            // REFACTOR_SPEC §11.3 saga/orchestrator gate (fakes)
    int repoFail = run_repository_tests();      // REFACTOR_SPEC §8.3 ICollectionRepository (domain objects)
    int importFail = run_import_service_tests(); // REFACTOR_SPEC P6 IImportService (curl/grpcurl/graphql)
    int prepoFail = run_persistence_repo_tests(); // REFACTOR_SPEC §6.3 env/session/appConfig repository ports
    int fieldFail = run_field_json_tests();      // REFACTOR_SPEC Phase E: domain field codec (JSON<->VO)

    fs::remove_all(root);

    std::printf("\n==== %d passed, %d failed (+%d stream, +%d ws, +%d sse, +%d gql, +%d mapper, +%d saga, +%d repo, +%d import, +%d prepo, +%d field) ====\n",
                g_pass, g_fail, streamFail, wsFail, sseFail, gqlFail, mapFail, sagaFail, repoFail, importFail, prepoFail, fieldFail);
    return (g_fail == 0 && streamFail == 0 && wsFail == 0 && sseFail == 0 && gqlFail == 0 &&
            mapFail == 0 && sagaFail == 0 && repoFail == 0 && importFail == 0 && prepoFail == 0 &&
            fieldFail == 0)
               ? 0
               : 1;
}
