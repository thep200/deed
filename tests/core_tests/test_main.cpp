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
int run_gql_introspection_tests();
int run_oauth2_provider_tests();
int run_avro_serde_tests();
int run_soap_tests();
// Defined in order_key_test.cpp — fractional index invariants (collection ordering). #failures.
int run_order_key_tests();

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

    // No back-compat: a name without an id is not our grammar -> rejected.
    CHECK(!parseRequestFilename("http_get_tours-configs.json").ok, "id-less name -> ok=false");

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
    // relPath keeps the on-disk folder name, which now carries an order prefix ("<key>+folder-a").
    CHECK(moved.rfind(folder + "/", 0) == 0, "move places file into target folder");

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

// ---------------- Collection ordering (fractional index prefix in the filename) ----------------
static void test_collection_ordering(const std::string& parentRoot) {
    std::printf("[collection_ordering]\n");
    std::string root = (fs::path(parentRoot) / "ordering").string();
    fs::create_directories(root);
    CollectionStore store(root);

    // display order of one level
    auto names = [&](const std::string& folderRel) {
        std::vector<std::string> out;
        for (const auto& n : store.scanLevel(folderRel)) out.push_back(n.name);
        return out;
    };
    // on-disk filenames of one level (to count how many entries a single op renames)
    auto files = [&](const std::string& folderRel) {
        std::vector<std::string> out;
        for (const auto& e : fs::directory_iterator(fs::path(root) / folderRel))
            out.push_back(e.path().filename().string());
        std::sort(out.begin(), out.end());
        return out;
    };
    auto joined = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) { if (!s.empty()) s += "|"; s += x; }
        return s;
    };

    // Created requests land at the BOTTOM, in creation order (not alphabetical).
    store.createRequest("", RequestType::Http, "Charlie");
    store.createRequest("", RequestType::Http, "Alpha");
    store.createRequest("", RequestType::Http, "Bravo");
    CHECK_EQ(joined(names("")), std::string("Charlie|Alpha|Bravo"), "new requests append at the end");

    // Drag "Bravo" to the top: index 0.
    std::string bravoRel;
    for (const auto& n : store.scanLevel("")) if (n.name == "Bravo") bravoRel = n.relPath;
    std::vector<std::string> before = files("");
    std::string movedRel = store.reorder(bravoRel, "", 0);
    CHECK_EQ(joined(names("")), std::string("Bravo|Charlie|Alpha"), "reorder to index 0");
    // Exactly ONE filename changed — the whole point of fractional indexing.
    std::vector<std::string> after = files("");
    int changed = 0;
    for (const auto& f : after)
        if (std::find(before.begin(), before.end(), f) == before.end()) ++changed;
    CHECK_EQ(changed, 1, "reorder renames exactly one file");
    CHECK(fs::exists(fs::path(root) / movedRel), "reorder returns the new relPath");

    // Insert into the middle repeatedly — still one rename each time.
    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha") bravoRel = n.relPath;
    store.reorder(bravoRel, "", 1);
    CHECK_EQ(joined(names("")), std::string("Bravo|Alpha|Charlie"), "reorder into the middle");

    // Dragging DOWN inside the same level: the index counts the list as the user sees it (with the
    // dragged row still in it), so "drop at slot 2" of [Bravo,Alpha,Charlie] lands between Alpha and
    // Charlie -> the row does not slide one slot too far.
    std::string downRel;
    for (const auto& n : store.scanLevel("")) if (n.name == "Bravo") downRel = n.relPath;
    store.reorder(downRel, "", 2);
    CHECK_EQ(joined(names("")), std::string("Alpha|Bravo|Charlie"), "drag down keeps the target slot");
    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha") downRel = n.relPath;
    store.reorder(downRel, "", 3);   // to the very end
    CHECK_EQ(joined(names("")), std::string("Bravo|Charlie|Alpha"), "drag to the end");

    // Folders share the sequence with requests (free interleaving).
    std::string fRel = store.createFolder("", "Zed");
    CHECK_EQ(joined(names("")), std::string("Bravo|Charlie|Alpha|zed"), "new folder appends at the end");
    fRel = store.reorder(fRel, "", 1);   // reorder renames the folder -> keep the new relPath
    CHECK_EQ(joined(names("")), std::string("Bravo|zed|Charlie|Alpha"), "folder can sit between requests");

    // Order survives a rename and a save (the key must not be dropped).
    std::string alphaRel;
    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha") alphaRel = n.relPath;
    store.rename(alphaRel, "Alpha Renamed");
    CHECK_EQ(joined(names("")), std::string("Bravo|zed|Charlie|Alpha renamed"), "rename keeps the slot");
    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha renamed") alphaRel = n.relPath;
    std::string savedRel = store.saveRequest(alphaRel, store.loadRequest(alphaRel));
    CHECK_EQ(joined(names("")), std::string("Bravo|zed|Charlie|Alpha renamed"), "save keeps the slot");
    (void)savedRel;

    // Duplicate lands right below its original.
    std::string charlieRel;
    for (const auto& n : store.scanLevel("")) if (n.name == "Charlie") charlieRel = n.relPath;
    store.reorder(charlieRel, "", 0);
    for (const auto& n : store.scanLevel("")) if (n.name == "Charlie") charlieRel = n.relPath;
    store.duplicate(charlieRel);
    CHECK_EQ(joined(names("")), std::string("Charlie|Charlie copy|Bravo|zed|Alpha renamed"),
             "duplicate sits under the original");
    // Cross-level drop uses the plain index (nothing to exclude) — no off-by-one there either.
    std::string intoFolder;
    for (const auto& n : store.scanLevel("")) if (n.name == "Bravo") intoFolder = n.relPath;
    store.reorder(intoFolder, fRel, 0);
    CHECK_EQ(joined(names(fRel)), std::string("Bravo"), "reorder into another level");
    CHECK_EQ(joined(names("")), std::string("Charlie|Charlie copy|zed|Alpha renamed"),
             "source level closes the gap");

    // move() (drop ONTO a folder, no slot) puts the entry at that folder's end, with a fresh key.
    std::string copyRel;
    for (const auto& n : store.scanLevel("")) if (n.name == "Charlie copy") copyRel = n.relPath;
    std::string inFolder = store.move(copyRel, fRel);
    CHECK(inFolder.rfind(fRel + "/", 0) == 0, "move into folder");
    CHECK(!splitOrderPrefix(fs::path(inFolder).filename().string()).order.empty(),
          "moved entry gets a key in its new level");

    // A file dropped in from outside the app carries no key. It is shown FIRST (sorted by name) and
    // must not break anything — the app ships without a migration path, so this is the only way an
    // unkeyed entry can appear.
    {
        std::ofstream f((fs::path(root) / "zz9foreignid0_http_get_foreign.json").string());
        f << "{\"id\":\"x\",\"name\":\"x\",\"type\":\"http\",\"http\":{\"method\":\"GET\",\"url\":\"\"}}";
    }
    CHECK_EQ(joined(names("")).rfind("Foreign|", 0), size_t(0), "unkeyed foreign file sorts first");
    std::string alphaAgain;
    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha renamed") alphaAgain = n.relPath;
    bool threw = false;
    try { (void)store.reorder(alphaAgain, "", 0); } catch (...) { threw = true; }
    CHECK(!threw, "reorder still works next to an unkeyed entry");
    for (const auto& n : store.scanLevel("")) {
        if (n.isFolder) continue;
        bool loadOk = true;
        try { (void)store.loadRequest(n.relPath); } catch (...) { loadOk = false; }
        CHECK(loadOk, "every sibling still loadable after reorder");
    }
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

// ---------------- Environment (plaintext) + migration ----------------
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
    // (env/alias rename = UI view-model save-new + delete-old)
}

// ---------------- Engine resolve + validate ----------------
static void test_engine(const std::string& root) {
    std::printf("[engine]\n");
    // prepare env Shared + active ("Global" reserved — see test_global_env).
    EnvironmentStore env(root);
    Environment g; g.name = "Shared"; g.keys.push_back({"baseUrl", "http://global", true});
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
    client->session().setActiveEnv("Shared");
    CHECK_EQ(client->resolvePreview("{{baseUrl}}/x"), std::string("http://global/x"), "active env Shared");
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
    // env "baseUrl" = http://global (active env is Shared at this point).
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

    // duplicate values -> first-defined key wins ("zdup" before "adup"). Save via client repo -> epoch
    // bump -> vars cache rebuilds.
    Environment go; go.name = "Shared";
    go.keys.push_back({"zdup", "http://dup.host", true});
    go.keys.push_back({"adup", "http://dup.host", true});
    client->environments().save(go);
    client->session().setActiveEnv("Shared");
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

// ---------------- Reserved "Global" env (always-on base layer) ----------------
static void test_global_env(const std::string& parentRoot) {
    std::printf("[global_env]\n");
    // isolated sub-root — earlier env files must not leak into the merge.
    std::string root = (fs::path(parentRoot) / "globalenv").string();
    fs::create_directories(root);

    {
        EnvironmentStore store(root);
        Environment g; g.name = "Global";
        g.keys.push_back({"baseUrl", "http://global", true});
        g.keys.push_back({"gOnly", "http://gonly", true});
        g.keys.push_back({"gOff", "nope", false});
        store.save(g);
        Environment d; d.name = "Dev";
        d.keys.push_back({"baseUrl", "http://dev", true});
        d.keys.push_back({"gOnly", "", true}); // blank cell — must NOT shadow Global
        d.keys.push_back({"dOnly", "http://donly", true});
        d.keys.push_back({"dEmpty", "", true});
        store.save(d);

        auto names = store.list();
        CHECK(std::find(names.begin(), names.end(), "Global") == names.end(), "list() hides reserved Global");
        CHECK(std::find(names.begin(), names.end(), "Dev") != names.end(), "list() still has ordinary envs");
        CHECK_EQ(store.load("Global").keys.size(), size_t(3), "load(Global) still works");
        std::uint64_t e0 = store.epoch();
        (void)store.list();
        CHECK_EQ(store.epoch(), e0, "list() does not bump epoch");
        store.save(g);
        CHECK(store.epoch() > e0, "save() bumps epoch");
    }

    auto client = core::app::CoreApiClient::create(
        core::app::CoreApiClient::Config{root, (fs::path(root) / "appconfig.json").string()});

    // always-on: no active env -> Global applies.
    CHECK_EQ(client->session().getActiveEnv(), std::string(""), "no active env by default");
    CHECK_EQ(client->resolvePreview("{{baseUrl}}/x"), std::string("http://global/x"),
             "Global applies with no active env");

    // reserved name never active.
    client->session().setActiveEnv("Global");
    CHECK_EQ(client->session().getActiveEnv(), std::string(""), "setActiveEnv(Global) is a no-op");

    // layering: active wins per key; blank cell falls through; union of both.
    client->session().setActiveEnv("Dev");
    CHECK_EQ(client->resolvePreview("{{baseUrl}}"), std::string("http://dev"), "active env overrides Global");
    CHECK_EQ(client->resolvePreview("{{gOnly}}"), std::string("http://gonly"),
             "blank active cell does not shadow Global");
    CHECK_EQ(client->resolvePreview("{{dOnly}}"), std::string("http://donly"), "active-only key resolves");
    CHECK_EQ(client->resolvePreview("a{{dEmpty}}b"), std::string("ab"), "active-only empty key -> \"\"");
    CHECK_EQ(client->resolvePreview("{{gOff}}"), std::string("{{gOff}}"), "disabled Global key ignored");

    // repo save bumps epoch -> vars cache rebuilds.
    Environment g2 = client->environments().load("Global");
    for (auto& k : g2.keys) if (k.key == "gOnly") k.value = "http://gonly2";
    client->environments().save(g2);
    CHECK_EQ(client->resolvePreview("{{gOnly}}"), std::string("http://gonly2"),
             "env edit invalidates the vars cache");

    // aliasify: shadowed Global pair must not capture literals; non-shadowed still aliasifies.
    namespace d2 = core::domain;
    const d2::RequestConfig cfg{d2::Timeout::fromMillis(1800000).take(), true};
    auto mkHttp = [&](const std::string& url) {
        d2::HttpRequest::Parts hp{d2::HttpMethod::Get, d2::Url::create(url).take(), d2::PathVariableList{},
                                  d2::QueryParamList{}, d2::HeaderList{}, d2::Body::none(), d2::Auth::none()};
        return d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                        d2::HttpRequest::create(std::move(hp)).take())
            .take();
    };
    auto shadowed = client->aliasifyModel(mkHttp("http://global/u"));
    CHECK_EQ(ts::http(shadowed).url().raw(), std::string("http://global/u"),
             "shadowed Global value not aliasified");
    auto activeWin = client->aliasifyModel(mkHttp("http://dev/u"));
    CHECK_EQ(ts::http(activeWin).url().raw(), std::string("{{baseUrl}}/u"), "active value aliasified");
    auto globalWin = client->aliasifyModel(mkHttp("http://gonly2/u"));
    CHECK_EQ(ts::http(globalWin).url().raw(), std::string("{{gOnly}}/u"), "Global-only value aliasified");

}

// ---------------- Env-value encryption (Enc toggle + encryption_key) ----------------
static void test_env_encryption(const std::string& parentRoot) {
    std::printf("[env_encryption]\n");
    std::string root = (fs::path(parentRoot) / "enc").string();
    fs::create_directories(root);

    AppConfigStore cfgStore((fs::path(root) / "config.json").string());
    AppConfig ac;
    ac.encryptionKey = "k1";
    cfgStore.save(ac);

    EnvironmentStore store(root);
    store.attachAppConfig(&cfgStore);

    auto rawFile = [&](const std::string& name) {
        std::string txt;
        fs::path p = fs::path(root) / "environments" / (name + ".json");
        std::FILE* f = std::fopen(p.string().c_str(), "rb");
        if (f) { char buf[8192]; size_t n = std::fread(buf, 1, sizeof(buf), f); txt.assign(buf, n); std::fclose(f); }
        return txt;
    };
    auto valOf = [](const Environment& e, const char* key) {
        for (const auto& k : e.keys) if (k.key == key) return k.value;
        return std::string();
    };

    Environment prod; prod.name = "Prod";
    prod.keys.push_back({"TOKEN", "hello-secret", true, true});   // Enc on
    prod.keys.push_back({"PLAIN", "world", true, false});
    store.save(prod);
    std::string raw = rawFile("Prod");
    CHECK(raw.find("hello-secret") == std::string::npos, "Enc value not plaintext on disk");
    CHECK(raw.find("enc:v1:") != std::string::npos, "Enc value carries enc:v1 marker");
    CHECK(raw.find("world") != std::string::npos, "non-Enc value stays plaintext");
    CHECK_EQ(valOf(store.load("Prod"), "TOKEN"), std::string("hello-secret"), "decrypt round-trip");

    // env name is irrelevant: any Enc-toggled value encrypts, "Local" included.
    Environment loc; loc.name = "Local";
    loc.keys.push_back({"TOKEN", "local-secret", true, true});
    store.save(loc);
    CHECK(rawFile("Local").find("local-secret") == std::string::npos, "Local env encrypts too");
    CHECK(rawFile("Local").find("enc:v1:") != std::string::npos, "Local env carries enc:v1 marker");

    // no key -> plaintext.
    cfgStore.save(AppConfig{});
    Environment nk; nk.name = "NoKey";
    nk.keys.push_back({"TOKEN", "nokey-secret", true, true});
    store.save(nk);
    CHECK(rawFile("NoKey").find("nokey-secret") != std::string::npos, "no key -> plaintext");

    // wrong key -> ciphertext kept (not lost, no crash).
    AppConfig wrong; wrong.encryptionKey = "k2"; cfgStore.save(wrong);
    CHECK(valOf(store.load("Prod"), "TOKEN").rfind("enc:v1:", 0) == 0, "wrong key -> ciphertext kept");

    // right key again -> decrypts.
    cfgStore.save(ac);
    CHECK_EQ(valOf(store.load("Prod"), "TOKEN"), std::string("hello-secret"), "right key decrypts again");

    // unchanged value re-saved -> stored ciphertext reused (no nonce re-roll, byte-stable file).
    std::string before = rawFile("Prod");
    store.save(store.load("Prod"));
    CHECK_EQ(rawFile("Prod"), before, "unchanged save keeps ciphertext bytes");

    // old plaintext + Enc flag: targeted re-save (the toggle/Back path) encrypts just that env.
    store.save(store.load("NoKey"));
    CHECK(rawFile("NoKey").find("nokey-secret") == std::string::npos, "targeted save encrypts old plaintext");
    CHECK(rawFile("NoKey").find("enc:v1:") != std::string::npos, "targeted save adds marker");
    CHECK_EQ(valOf(store.load("NoKey"), "TOKEN"), std::string("nokey-secret"), "and it decrypts back");

    // Key change needs no migration pass: the GCM tag makes a wrong key a hard failure (never garbage),
    // so an undecryptable value is carried through verbatim and the original key still recovers it.
    cfgStore.save(ac);
    store.save(store.load("Prod"));          // -> encrypted under k1
    AppConfig k3 = ac; k3.encryptionKey = "k3";
    cfgStore.save(k3);
    Environment stale = store.load("Prod");
    CHECK(valOf(stale, "TOKEN").rfind("enc:v1:", 0) == 0, "wrong key -> ciphertext, never garbage");
    for (auto& k : stale.keys) if (k.key == "TOKEN") k.secret = false;   // Enc toggled OFF under wrong key
    store.save(stale);
    cfgStore.save(ac);
    CHECK_EQ(valOf(store.load("Prod"), "TOKEN"), std::string("hello-secret"), "original key still recovers");
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

    AppConfig defaults;
    defaults.fontName = "Courier";
    defaults.fontSize = 17;
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
    CHECK_EQ(pc.ramCacheSizeMb, 33, "missing key -> ram default .env");
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
        const auto& badH = impv::httpOf(bad);
        CHECK(bad.ok && impv::authView(badH.auth()).type == "none" && badH.headers().size() == 1 &&
                  badH.headers().items()[0].name() == "Authorization",
              "M15: malformed Basic -> raw Authorization header kept (not garbled creds)");
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
    test_response_cache(root);
    test_cache_durability(root);
    test_cache_config_clamp(root);
    test_app_config_defaults(root);
    test_collection_store(root);
    test_body_singlemode_roundtrip(root);
    test_collection_ordering(root);
    test_session_store(root);
    test_env_and_secret(root);
    test_engine(root);
    test_global_env(root);
    test_env_encryption(root);
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
    int introFail = run_gql_introspection_tests(); // GraphQL introspection SDL printer (Schema tab)
    int oauthFail = run_oauth2_provider_tests();   // OAuth2 token provider (pure pieces, no network)
    int avroFail = run_avro_serde_tests();         // Kafka Avro framing/serde + SR response parsing
    int soapFail = run_soap_tests();               // SOAP HTTP packaging + fault extract + XML pretty
    int orderFail = run_order_key_tests();         // fractional index keys (collection ordering)

    fs::remove_all(root);

    std::printf("\n==== %d passed, %d failed (+%d stream, +%d ws, +%d sse, +%d gql, +%d mapper, +%d saga, +%d repo, +%d import, +%d prepo, +%d field, +%d intro, +%d oauth, +%d avro, +%d soap, +%d order) ====\n",
                g_pass, g_fail, streamFail, wsFail, sseFail, gqlFail, mapFail, sagaFail, repoFail, importFail, prepoFail, fieldFail, introFail, oauthFail, avroFail, soapFail, orderFail);
    return (g_fail == 0 && streamFail == 0 && wsFail == 0 && sseFail == 0 && gqlFail == 0 &&
            mapFail == 0 && sagaFail == 0 && repoFail == 0 && importFail == 0 && prepoFail == 0 &&
            fieldFail == 0 && introFail == 0 && oauthFail == 0 && avroFail == 0 && soapFail == 0 &&
            orderFail == 0)
               ? 0
               : 1;
}
