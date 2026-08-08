#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/app/core_api_client.hpp" // domain stack facade (replaces Engine in these tests)
#include "core/infra/persistence/request_naming.hpp"
#include "core/infra/persistence/stores.hpp"

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

namespace { // internal linkage: engine_import_test.cpp carries its own ts subset
namespace ts {
namespace d = core::domain;
const d::HttpRequest& http(const d::RequestModel& m) { return std::get<d::HttpRequest>(m.payload()); }
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
} // namespace

// Domain Body is single-variant: only the ACTIVE mode persists — inactive mode content is dropped by design.
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

    std::vector<core::domain::FormField> ff{{"f1", "v1", true}, {"f2", "v2", false}};
    auto f = ts::withHttp(store.loadRequest(rel), core::domain::HttpMethod::Get, "",
                          core::domain::Body::formUrlEncoded(ff));
    rel = store.saveRequest(rel, f);
    auto fr = store.loadRequest(rel);
    CHECK_EQ(ts::bodyMode(ts::http(fr).body()), std::string("form-urlencoded"), "active form mode round-trip");

    // Body drafts: active=json persists in the domain Body; the non-active form draft rides in "_uiBodyDrafts" so a mode switch after reload isn't empty.
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
    // unique ids + findRelPathById (regression: duplicate ids once deleted the wrong file).
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
    bool uaOk = false, ctEnabled = false;
    for (const auto& h : ts::http(m).headers().items()) {
        if (h.name() == "User-Agent") uaOk = (h.value() == "deed" && h.enabled());
        if (h.name() == "Content-Type") ctEnabled = h.enabled();
    }
    CHECK(uaOk, "User-Agent=deed enabled on new request");
    CHECK(!ctEnabled, "other hint headers remain disabled");

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

    std::string folder = store.createFolder("", "Folder A");
    CHECK(fs::is_directory(fs::path(root) / folder), "folder created");
    std::string nested = store.createRequest(folder, RequestType::Grpc, "Get User");
    auto gm = store.loadRequest(nested);
    CHECK(gm.type() == core::domain::RequestType::Grpc, "nested = grpc");

    TreeNode tree = store.scanTree();
    CHECK(tree.isFolder, "root is a folder");
    bool foundFolder = false;
    for (const auto& c : tree.children) if (c.isFolder && c.name == "folder-a") foundFolder = true;
    CHECK(foundFolder, "tree has child folder");

    std::vector<TreeNode> rootLevel = store.scanLevel("");
    bool folderLazy = false, reqLabelOk = false;
    for (const auto& c : rootLevel) {
        if (c.isFolder && c.name == "folder-a") {
            folderLazy = c.children.empty();
        } else if (!c.isFolder) {
            if (c.name == "Get users") { reqLabelOk = (c.methodOrType == "POST"); }
        }
    }
    CHECK(folderLazy, "scanLevel: child folder not loaded (lazy)");
    CHECK(reqLabelOk, "scanLevel: leaf name = de-slug, badge = method from filename");

    std::vector<TreeNode> inFolder = store.scanLevel(folder);
    bool grpcLeafOk = false;
    for (const auto& c : inFolder)
        if (!c.isFolder && c.requestType == RequestType::Grpc && c.name == "Get user") grpcLeafOk = true;
    CHECK(grpcLeafOk, "scanLevel folder: grpc leaf name = 'Get user' (no grpc_ prefix)");

    std::string toMove = store.createRequest("", RequestType::Http, "Movable");
    std::string moved = store.move(toMove, folder);
    CHECK(!fs::exists(fs::path(root) / toMove), "old file gone after move");
    CHECK(fs::exists(fs::path(root) / moved), "new file exists in folder");
    // relPath keeps the on-disk folder name, which now carries an order prefix ("<key>+folder-a").
    CHECK(moved.rfind(folder + "/", 0) == 0, "move places file into target folder");

    std::string dup = store.duplicate(rel);
    CHECK(fs::exists(fs::path(root) / dup), "duplicate creates file");
    std::string renamed = store.rename(dup, "Renamed Req");
    CHECK(fs::exists(fs::path(root) / renamed), "rename creates new file");
    store.remove(renamed);
    CHECK(!fs::exists(fs::path(root) / renamed), "remove deletes file");

    store.ensureGitignore();
    std::string gi;
    fs::path gip = fs::path(root) / ".gitignore";
    CHECK(fs::exists(gip), ".gitignore created");
}

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

    store.createRequest("", RequestType::Http, "Charlie");
    store.createRequest("", RequestType::Http, "Alpha");
    store.createRequest("", RequestType::Http, "Bravo");
    CHECK_EQ(joined(names("")), std::string("Charlie|Alpha|Bravo"), "new requests append at the end");

    std::string bravoRel;
    for (const auto& n : store.scanLevel("")) if (n.name == "Bravo") bravoRel = n.relPath;
    std::vector<std::string> before = files("");
    std::string movedRel = store.reorder(bravoRel, "", 0);
    CHECK_EQ(joined(names("")), std::string("Bravo|Charlie|Alpha"), "reorder to index 0");
    std::vector<std::string> after = files("");
    int changed = 0;
    for (const auto& f : after)
        if (std::find(before.begin(), before.end(), f) == before.end()) ++changed;
    CHECK_EQ(changed, 1, "reorder renames exactly one file");
    CHECK(fs::exists(fs::path(root) / movedRel), "reorder returns the new relPath");

    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha") bravoRel = n.relPath;
    store.reorder(bravoRel, "", 1);
    CHECK_EQ(joined(names("")), std::string("Bravo|Alpha|Charlie"), "reorder into the middle");

    // Drag-down index counts the list WITH the dragged row still in it, so the row does not land one slot too far.
    std::string downRel;
    for (const auto& n : store.scanLevel("")) if (n.name == "Bravo") downRel = n.relPath;
    store.reorder(downRel, "", 2);
    CHECK_EQ(joined(names("")), std::string("Alpha|Bravo|Charlie"), "drag down keeps the target slot");
    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha") downRel = n.relPath;
    store.reorder(downRel, "", 3);
    CHECK_EQ(joined(names("")), std::string("Bravo|Charlie|Alpha"), "drag to the end");

    std::string fRel = store.createFolder("", "Zed");
    CHECK_EQ(joined(names("")), std::string("Bravo|Charlie|Alpha|zed"), "new folder appends at the end");
    fRel = store.reorder(fRel, "", 1);   // reorder renames the folder -> keep the new relPath
    CHECK_EQ(joined(names("")), std::string("Bravo|zed|Charlie|Alpha"), "folder can sit between requests");

    std::string alphaRel;
    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha") alphaRel = n.relPath;
    store.rename(alphaRel, "Alpha Renamed");
    CHECK_EQ(joined(names("")), std::string("Bravo|zed|Charlie|Alpha renamed"), "rename keeps the slot");
    for (const auto& n : store.scanLevel("")) if (n.name == "Alpha renamed") alphaRel = n.relPath;
    std::string savedRel = store.saveRequest(alphaRel, store.loadRequest(alphaRel));
    CHECK_EQ(joined(names("")), std::string("Bravo|zed|Charlie|Alpha renamed"), "save keeps the slot");
    (void)savedRel;

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

    // A file dropped in from outside the app carries no order key: it sorts first by name and must not break anything (no migration path exists).
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

// A level written before fractional indexing carries no order keys, and keyless entries sort FIRST —
// so a freshly keyed entry could only ever land below them: dragging up was a silent no-op and
// dragging down always jumped to the bottom. The first drop has to key the whole level.
static void test_collection_ordering_legacy_level(const std::string& parentRoot) {
    std::printf("[collection_ordering_legacy]\n");
    std::string root = (fs::path(parentRoot) / "legacy-ordering").string();
    fs::create_directories(root);
    CollectionStore store(root);

    auto joined = [&](const std::string& folderRel) {
        std::string s;
        for (const auto& n : store.scanLevel(folderRel)) { if (!s.empty()) s += "|"; s += n.name; }
        return s;
    };
    auto files = [&] {
        std::vector<std::string> out;
        for (const auto& e : fs::directory_iterator(root)) out.push_back(e.path().filename().string());
        std::sort(out.begin(), out.end());
        return out;
    };
    auto relOf = [&](const std::string& name) {
        for (const auto& n : store.scanLevel("")) if (n.name == name) return n.relPath;
        return std::string();
    };
    auto keyless = [&] {
        int n = 0;
        for (const auto& e : store.scanLevel(""))
            if (splitOrderPrefix(fs::path(e.relPath).filename().string()).order.empty()) ++n;
        return n;
    };
    auto renameCount = [](const std::vector<std::string>& before, const std::vector<std::string>& after) {
        int n = 0;
        for (const auto& f : after)
            if (std::find(before.begin(), before.end(), f) == before.end()) ++n;
        return n;
    };

    const char* slugs[] = {"alpha", "bravo", "charlie", "delta", "echo"};
    for (int i = 0; i < 5; ++i) {
        std::string name = "legacyid000" + std::string(1, char('a' + i)) + "_http_get_" + slugs[i] + ".json";
        std::ofstream f((fs::path(root) / name).string());
        f << "{\"id\":\"legacyid000" << char('a' + i)
          << "\",\"name\":\"" << slugs[i] << "\",\"type\":\"http\",\"http\":{\"method\":\"GET\",\"url\":\"\"}}";
    }
    fs::create_directories(fs::path(root) / "legacy-folder");
    CHECK_EQ(joined(""), std::string("Alpha|Bravo|Charlie|Delta|Echo|legacy-folder"),
             "legacy level sorts by display name");
    CHECK_EQ(keyless(), 6, "legacy level starts with no order keys");

    std::vector<std::string> before = files();
    store.reorder(relOf("Echo"), "", 0);   // drag UP — used to be a silent no-op
    CHECK_EQ(joined(""), std::string("Echo|Alpha|Bravo|Charlie|Delta|legacy-folder"), "drag to the top");
    CHECK_EQ(keyless(), 0, "the first drop keys the whole level");
    CHECK_EQ(renameCount(before, files()), 6, "backfill renames every entry of that level, once");

    before = files();
    store.reorder(relOf("Alpha"), "", 3);
    CHECK_EQ(joined(""), std::string("Echo|Bravo|Alpha|Charlie|Delta|legacy-folder"), "drag down into the middle");
    CHECK_EQ(renameCount(before, files()), 1, "a keyed level is back to one rename per drop");

    store.reorder(relOf("legacy-folder"), "", 0);
    CHECK_EQ(joined(""), std::string("legacy-folder|Echo|Bravo|Alpha|Charlie|Delta"),
             "folders share the same key sequence");

    for (const auto& n : store.scanLevel("")) {
        if (n.isFolder) continue;
        bool loadOk = true;
        try { (void)store.loadRequest(n.relPath); } catch (...) { loadOk = false; }
        CHECK(loadOk, "backfill only touches the key prefix");
    }
}

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

    CHECK_EQ(client->session().getActiveEnv(), std::string(""), "no active env by default");
    CHECK_EQ(client->resolvePreview("{{baseUrl}}/x"), std::string("http://global/x"),
             "Global applies with no active env");

    client->session().setActiveEnv("Global");
    CHECK_EQ(client->session().getActiveEnv(), std::string(""), "setActiveEnv(Global) is a no-op");

    client->session().setActiveEnv("Dev");
    CHECK_EQ(client->resolvePreview("{{baseUrl}}"), std::string("http://dev"), "active env overrides Global");
    CHECK_EQ(client->resolvePreview("{{gOnly}}"), std::string("http://gonly"),
             "blank active cell does not shadow Global");
    CHECK_EQ(client->resolvePreview("{{dOnly}}"), std::string("http://donly"), "active-only key resolves");
    CHECK_EQ(client->resolvePreview("a{{dEmpty}}b"), std::string("ab"), "active-only empty key -> \"\"");
    CHECK_EQ(client->resolvePreview("{{gOff}}"), std::string("{{gOff}}"), "disabled Global key ignored");

    Environment g2 = client->environments().load("Global");
    for (auto& k : g2.keys) if (k.key == "gOnly") k.value = "http://gonly2";
    client->environments().save(g2);
    CHECK_EQ(client->resolvePreview("{{gOnly}}"), std::string("http://gonly2"),
             "env edit invalidates the vars cache");

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

    Environment loc; loc.name = "Local";
    loc.keys.push_back({"TOKEN", "local-secret", true, true});
    store.save(loc);
    CHECK(rawFile("Local").find("local-secret") == std::string::npos, "Local env encrypts too");
    CHECK(rawFile("Local").find("enc:v1:") != std::string::npos, "Local env carries enc:v1 marker");

    cfgStore.save(AppConfig{});
    Environment nk; nk.name = "NoKey";
    nk.keys.push_back({"TOKEN", "nokey-secret", true, true});
    store.save(nk);
    CHECK(rawFile("NoKey").find("nokey-secret") != std::string::npos, "no key -> plaintext");

    AppConfig wrong; wrong.encryptionKey = "k2"; cfgStore.save(wrong);
    CHECK(valOf(store.load("Prod"), "TOKEN").rfind("enc:v1:", 0) == 0, "wrong key -> ciphertext kept");

    cfgStore.save(ac);
    CHECK_EQ(valOf(store.load("Prod"), "TOKEN"), std::string("hello-secret"), "right key decrypts again");

    std::string before = rawFile("Prod");
    store.save(store.load("Prod"));
    CHECK_EQ(rawFile("Prod"), before, "unchanged save keeps ciphertext bytes");

    store.save(store.load("NoKey"));
    CHECK(rawFile("NoKey").find("nokey-secret") == std::string::npos, "targeted save encrypts old plaintext");
    CHECK(rawFile("NoKey").find("enc:v1:") != std::string::npos, "targeted save adds marker");
    CHECK_EQ(valOf(store.load("NoKey"), "TOKEN"), std::string("nokey-secret"), "and it decrypts back");

    // Key change needs no migration pass: GCM makes a wrong key a hard failure, so undecryptable values carry through verbatim.
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

int run_persistence_store_tests() {
    std::string root = makeTempRoot();

    test_collection_store(root);
    test_body_singlemode_roundtrip(root);
    test_collection_ordering(root);
    test_collection_ordering_legacy_level(root);
    test_session_store(root);
    test_env_and_secret(root);
    test_global_env(root);
    test_env_encryption(root);

    fs::remove_all(root);
    std::printf("  persistence_store: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
