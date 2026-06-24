#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>

#include "core/persistence/stores.hpp"
#include "core/persistence/request_naming.hpp"
#include "infra/fs_util.hpp"
#include "codec/json_codec.hpp"

namespace fs = std::filesystem;

namespace core {

namespace {

// Special dirs that are NOT shown in the request tree.
bool isReservedDir(const std::string& name) {
    return name == ".session" || name == ".secrets" || name == ".git" ||
           name == "environments";
}
// Collection/folder-level config file -> not a request.
bool isConfigFile(const std::string& name) {
    return name == "collection.json" || name == "folder.json";
}
bool isHidden(const std::string& name) { return !name.empty() && name[0] == '.'; }

// Generate a lightweight random id: base36 LOWERCASE, 12 chars, NO '_' (to embed in FILENAME — §2A).
// RNG seeded ONCE then advances on each call -> no collisions even on back-to-back calls.
std::string genId() {
    static std::mutex mu;
    static std::mt19937_64 rng([] {
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        s ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return s;
    }());
    static const char* alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"; // base36, [a-z0-9]
    std::lock_guard<std::mutex> lk(mu);
    std::string s;
    for (int i = 0; i < 12; ++i) s += alphabet[rng() % 36];
    return s;
}

// id usable in a filename? (legacy "req_..." contains '_' -> invalid -> must regenerate on migrate).
std::string ensureFileId(const std::string& id) { return isValidFileId(id) ? id : genId(); }

std::string uniquePath(const fs::path& dir, const std::string& slug, const std::string& ext) {
    fs::path cand = dir / (slug + ext);
    if (!fs::exists(cand)) return cand.string();
    for (int i = 2; i < 10000; ++i) {
        fs::path c = dir / (slug + "-" + std::to_string(i) + ext);
        if (!fs::exists(c)) return c.string();
    }
    throw std::runtime_error("could not find a unique filename for: " + slug);
}

// Find a unique filename per the encoded grammar (<id>_type_..., id first). Unique ids almost
// never collide; still keep a slug-suffix loop to be safe. Returns FILENAME (no directory).
std::string uniqueEncodedName(const fs::path& dir, const std::string& id, RequestType type,
                              const std::string& method, const std::string& displayName) {
    std::string first = encodeRequestFilename(id, type, method, displayName);
    if (!fs::exists(dir / first)) return first;
    for (int i = 2; i < 10000; ++i) {
        std::string cand = encodeRequestFilename(id, type, method, displayName + " " + std::to_string(i));
        if (!fs::exists(dir / cand)) return cand;
    }
    throw std::runtime_error("could not find a unique filename for: " + displayName);
}

// Build metadata leaf for one request file — does NOT read content when the filename is valid (§2).
// Only falls back to a single read if the filename violates the grammar (§5).
TreeNode buildRequestLeaf(const fs::path& fullPath, const std::string& relPath) {
    TreeNode leaf;
    leaf.isFolder = false;
    leaf.relPath = relPath;
    std::string fname = fullPath.filename().string();

    ParsedRequestName p = parseRequestFilename(fname);
    if (p.ok) {
        leaf.requestType = p.type;
        leaf.name = normalizeDisplayName(p.slug);
        if (p.type == RequestType::Http) {
            std::string m = p.method;
            for (auto& c : m) c = static_cast<char>(std::toupper((unsigned char)c));
            leaf.methodOrType = m;            // badge HTTP method (GET/POST...)
        } else {
            leaf.methodOrType.clear();        // gRPC: UI does not show a method type
        }
        leaf.id = p.id;                       // id READ FROM FILENAME (zero-read) — used for reveal/cache.
        if (leaf.id.empty()) {                // OLD file with no id in name -> read once for content id
            std::string txt;
            if (fsutil::readFile(fullPath.string(), txt)) {
                try { leaf.id = codec::json::parse(txt).value("id", std::string()); } catch (...) {}
            }
        }
        return leaf;
    }

    // Fallback: filename violates the grammar -> read once for the real type/method/name.
    leaf.name = fullPath.stem().string();     // at minimum: filename (without .json)
    std::string txt;
    if (fsutil::readFile(fullPath.string(), txt)) {
        try {
            auto j = codec::json::parse(txt);
            if (j.contains("name") && j["name"].is_string())
                leaf.name = j["name"].get<std::string>();
            leaf.id = j.value("id", std::string());
            std::string t = j.value("type", "http");
            parseRequestType(t, leaf.requestType);
            if (leaf.requestType == RequestType::Http)
                leaf.methodOrType = j.value("http", codec::json::object()).value("method", "GET");
            else
                leaf.methodOrType.clear();
        } catch (...) { /* bad file -> still show the filename */ }
    }
    return leaf;
}

} // namespace

CollectionStore::CollectionStore(std::string root) : root_(std::move(root)) {}
void CollectionStore::setRoot(std::string root) { root_ = std::move(root); invalidateIdIndex(); }

// Scan one level — metadata-only. Subfolders kept folded (empty children). Does NOT read content
// to render (only the buildRequestLeaf fallback when a filename violates the grammar). §3.
std::vector<TreeNode> CollectionStore::scanLevel(const std::string& dirRelPath) const {
    std::vector<TreeNode> out;
    fs::path dir = fs::path(fsutil::join(root_, dirRelPath));
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        // Do not follow symlinks (avoid recursion cycles — §10).
        if (e.is_symlink()) continue;
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.is_directory() != b.is_directory()) return a.is_directory(); // folders first
        return a.path().filename().string() < b.path().filename().string();
    });
    for (const auto& e : entries) {
        std::string fname = e.path().filename().string();
        std::string childRel = dirRelPath.empty() ? fname : dirRelPath + "/" + fname;
        if (e.is_directory()) {
            if (isReservedDir(fname) || isHidden(fname)) continue;
            TreeNode folder;
            folder.isFolder = true;
            folder.relPath = childRel;
            folder.name = fname;                  // folder: directory name (NO de-slug)
            out.push_back(std::move(folder));     // empty children -> lazy expand later
        } else if (e.is_regular_file()) {
            if (e.path().extension() != ".json") continue;
            if (isConfigFile(fname) || isHidden(fname)) continue;
            out.push_back(buildRequestLeaf(e.path(), childRel));
        }
    }
    return out;
}

TreeNode CollectionStore::scanTree() const {
    std::function<TreeNode(const std::string&)> walk =
        [&](const std::string& rel) -> TreeNode {
        TreeNode node;
        node.isFolder = true;
        node.relPath = rel;
        node.name = rel.empty() ? fs::path(root_).filename().string()
                                : fs::path(rel).filename().string();
        for (auto& child : scanLevel(rel)) {
            if (child.isFolder) node.children.push_back(walk(child.relPath));
            else node.children.push_back(std::move(child));
        }
        return node;
    };
    return walk("");
}

RequestModel CollectionStore::loadRequest(const std::string& relPath) const {
    std::string txt;
    if (!fsutil::readFile(fsutil::join(root_, relPath), txt))
        throw std::runtime_error("cannot read request: " + relPath);
    RequestModel m = codec::requestFromJson(codec::json::parse(txt));
    // id must be valid to embed in the FILENAME ([a-z0-9], no '_'). Empty/legacy "req_..." content
    // -> do NOT write to disk here (loadRequest is a pure READ — avoid write-storm on open/browse).
    // Prefer the id FROM THE FILENAME (stable after migrateAddIdToFilenames); only generate a temp
    // one if the filename also lacks an id. Content rewrites are owned by saveRequest/migrateAddIdToFilenames.
    if (!isValidFileId(m.id)) {
        ParsedRequestName p = parseRequestFilename(fs::path(relPath).filename().string());
        m.id = (p.ok && isValidFileId(p.id)) ? p.id : genId();
    }
    return m;
}

// Build idIndex_ in one pass: id preferred FROM FILENAME (zero-read), read content only for legacy files.
void CollectionStore::buildIdIndexLocked() const {
    idIndex_.clear();
    std::function<void(const std::string&)> walk = [&](const std::string& rel) {
        fs::path dir = fs::path(fsutil::join(root_, rel));
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_symlink()) continue;
            std::string fname = e.path().filename().string();
            std::string childRel = rel.empty() ? fname : rel + "/" + fname;
            if (e.is_directory()) {
                if (isReservedDir(fname) || isHidden(fname)) continue;
                walk(childRel);
            } else if (e.is_regular_file()) {
                if (e.path().extension() != ".json" || isConfigFile(fname) || isHidden(fname))
                    continue;
                ParsedRequestName p = parseRequestFilename(fname);
                std::string id = p.id;
                if (id.empty()) {                    // legacy file with no id in name -> read content
                    std::string txt;
                    if (fsutil::readFile(e.path().string(), txt)) {
                        try { id = codec::json::parse(txt).value("id", std::string()); } catch (...) {}
                    }
                }
                if (!id.empty()) idIndex_.emplace(id, childRel);  // first id wins (stable)
            }
        }
    };
    walk("");
    idIndexBuilt_ = true;
}

void CollectionStore::invalidateIdIndex() const {
    std::lock_guard<std::mutex> lk(idMu_);
    idIndexBuilt_ = false;
}

std::string CollectionStore::findRelPathById(const std::string& id) const {
    if (id.empty()) return "";
    std::lock_guard<std::mutex> lk(idMu_);
    if (!idIndexBuilt_) buildIdIndexLocked();
    auto it = idIndex_.find(id);
    if (it == idIndex_.end()) return "";
    // Validate: does the file still exist at the cached path? (guard against changes OUTSIDE the app)
    // -> if gone, rebuild once and re-look up; avoid returning a "ghost" path.
    if (fs::exists(fs::path(fsutil::join(root_, it->second)))) return it->second;
    buildIdIndexLocked();
    auto it2 = idIndex_.find(id);
    return it2 != idIndex_.end() ? it2->second : "";
}

int CollectionStore::migrateAddIdToFilenames() const {
    int migrated = 0;
    std::function<void(const std::string&)> walk = [&](const std::string& rel) {
        fs::path dir = fs::path(fsutil::join(root_, rel));
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_symlink()) continue;
            std::string fname = e.path().filename().string();
            std::string childRel = rel.empty() ? fname : rel + "/" + fname;
            if (e.is_directory()) {
                if (isReservedDir(fname) || isHidden(fname)) continue;
                walk(childRel);
            } else if (e.is_regular_file()) {
                if (e.path().extension() != ".json" || isConfigFile(fname) || isHidden(fname))
                    continue;
                ParsedRequestName p = parseRequestFilename(fname);
                if (p.ok && !p.id.empty()) continue;     // already has id in name -> do NOT read content
                files.push_back(childRel);               // needs migrate (handle after the dir loop)
            }
        }
        for (const auto& childRel : files) {
            try {
                RequestModel m = loadRequest(childRel);  // ensure a clean id (generate if legacy/empty)
                std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
                fs::path src = fs::path(fsutil::join(root_, childRel));
                std::string newName = uniqueEncodedName(src.parent_path(), m.id, m.type, method, m.name);
                if (newName != src.filename().string()) {
                    fs::rename(src, src.parent_path() / newName, ec);
                    if (!ec) ++migrated;
                }
            } catch (...) { /* bad file -> skip, do not block migrating the rest */ }
        }
    };
    walk("");
    if (migrated) invalidateIdIndex();   // renamed files -> idIndex_ (keyed by relPath) is now stale
    return migrated;
}

std::string CollectionStore::saveRequest(const std::string& relPath, const RequestModel& m) const {
    invalidateIdIndex();   // may rename file (relPath changes) -> idIndex_ must be rebuilt
    fs::path cur = fs::path(fsutil::join(root_, relPath));
    fs::path dir = cur.parent_path();
    std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
    // Write content first (source of truth), then sync the filename = derived cache (§4).
    fsutil::writeFileAtomic(cur.string(), codec::dumpRequest(m));
    std::string desired = encodeRequestFilename(m.id, m.type, method, m.name);
    if (cur.filename().string() == desired) return relPath;   // name already matches -> done
    std::string newName = uniqueEncodedName(dir, m.id, m.type, method, m.name);
    fs::path dst = dir / newName;
    std::error_code ec;
    fs::rename(cur, dst, ec);                  // git detects rename via unchanged content
    if (ec) return relPath;                    // rename failed -> keep old path (content already written)
    return fs::relative(dst, fs::path(root_)).generic_string();
}

std::string CollectionStore::createRequest(const std::string& folderRel, RequestType type,
                                           const std::string& name) const {
    invalidateIdIndex();
    fs::path dir = fs::path(fsutil::join(root_, folderRel));
    fs::create_directories(dir);
    RequestModel m;
    m.id = genId();
    m.name = name;
    m.type = type;
    if (type == RequestType::Http) {
        m.http.method = "GET";
        // Common default headers offered as hints, OFF BY DEFAULT (enabled=false); the user enables
        // each as needed (like how Postman ships headers disabled). EXCEPTION: User-Agent="deed" is
        // enabled so every NEW request identifies itself. This applies to freshly created requests
        // only — existing requests (loaded via load/save) and imports are never auto-modified.
        m.http.headers.push_back({"Content-Type", "application/json", false});
        m.http.headers.push_back({"Accept", "*/*", false});
        m.http.headers.push_back({"User-Agent", "deed", true});
        m.http.headers.push_back({"Accept-Encoding", "gzip, deflate, br", false});
        m.http.headers.push_back({"Connection", "keep-alive", false});
        m.http.body.mode = "none";
    } else if (type == RequestType::WebSocket) {
        m.ws.defaultSendKind = WsSendKind::Text;   // url filled in by the user (ws:// or wss://)
    } else if (type == RequestType::GraphQL) {
        m.graphql.query = "query {\n  \n}";        // starter document
        m.graphql.variablesJson = "{}";
    } else {
        m.grpc.methodType = "unary";
        m.grpc.protoSource.mode = "reflection";
        m.grpc.message = "{}";
    }
    std::string method = (type == RequestType::Http) ? m.http.method : std::string();
    fs::path full = dir / uniqueEncodedName(dir, m.id, type, method, name);
    fsutil::writeFileAtomic(full.string(), codec::dumpRequest(m));
    return fs::relative(full, fs::path(root_)).generic_string();
}

std::string CollectionStore::createRequestFromModel(const std::string& folderRel, RequestModel m,
                                                    const std::string& name) const {
    invalidateIdIndex();
    fs::path dir = fs::path(fsutil::join(root_, folderRel));
    fs::create_directories(dir);
    m.id = genId();      // new id, independent of the import source
    m.name = name;
    std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
    fs::path full = dir / uniqueEncodedName(dir, m.id, m.type, method, name);
    fsutil::writeFileAtomic(full.string(), codec::dumpRequest(m));
    return fs::relative(full, fs::path(root_)).generic_string();
}

std::string CollectionStore::createFolder(const std::string& parentRel, const std::string& name) const {
    invalidateIdIndex();
    std::string slug = fsutil::slugify(name);
    fs::path dir = fs::path(fsutil::join(fsutil::join(root_, parentRel), slug));
    fs::create_directories(dir);
    return fs::relative(dir, fs::path(root_)).generic_string();
}

std::string CollectionStore::rename(const std::string& relPath, const std::string& newName) const {
    invalidateIdIndex();
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("does not exist: " + relPath);
    if (fs::is_directory(src)) {
        fs::path dst = src.parent_path() / fsutil::slugify(newName);
        fs::rename(src, dst);
        return fs::relative(dst, fs::path(root_)).generic_string();
    }
    // request: update the name field + rename the file (KEEP old id, change only the slug). §2A.
    RequestModel m = loadRequest(relPath);
    m.name = newName;
    std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
    fs::path newFull = src.parent_path() / uniqueEncodedName(src.parent_path(), m.id, m.type, method, newName);
    fsutil::writeFileAtomic(newFull.string(), codec::dumpRequest(m));
    fs::remove(src);
    return fs::relative(newFull, fs::path(root_)).generic_string();
}

std::string CollectionStore::duplicate(const std::string& relPath) const {
    invalidateIdIndex();
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("does not exist: " + relPath);
    if (fs::is_directory(src)) {
        fs::path dst = src.parent_path() / (src.filename().string() + "-copy");
        std::error_code ec;
        fs::copy(src, dst, fs::copy_options::recursive, ec);
        if (ec) throw std::runtime_error("duplicate folder error: " + ec.message());
        return fs::relative(dst, fs::path(root_)).generic_string();
    }
    RequestModel m = loadRequest(relPath);
    m.id = genId();                 // new id to avoid collisions
    m.name = m.name + " copy";
    std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
    fs::path newFull = src.parent_path() / uniqueEncodedName(src.parent_path(), m.id, m.type, method, m.name);
    fsutil::writeFileAtomic(newFull.string(), codec::dumpRequest(m));
    return fs::relative(newFull, fs::path(root_)).generic_string();
}

std::string CollectionStore::move(const std::string& relPath, const std::string& destFolderRel) const {
    invalidateIdIndex();
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("does not exist: " + relPath);
    fs::path destDir = fs::path(fsutil::join(root_, destFolderRel));
    fs::create_directories(destDir);

    // Block moving a folder into itself / its descendants.
    if (fs::is_directory(src)) {
        auto s = fs::weakly_canonical(src);
        auto d = fs::weakly_canonical(destDir);
        auto mismatch = std::mismatch(s.begin(), s.end(), d.begin(), d.end());
        if (mismatch.first == s.end()) throw std::runtime_error("cannot move a folder into itself");
    }
    // Do nothing if already in the right folder.
    if (fs::equivalent(src.parent_path(), destDir)) return relPath;

    std::string stem = src.stem().string();
    std::string ext = src.has_extension() ? src.extension().string() : "";
    fs::path dest = fs::is_directory(src) ? (destDir / src.filename())
                                          : fs::path(uniquePath(destDir, stem, ext));
    fs::rename(src, dest);
    return fs::relative(dest, fs::path(root_)).generic_string();
}

void CollectionStore::remove(const std::string& relPath) const {
    invalidateIdIndex();
    fs::path p = fs::path(fsutil::join(root_, relPath));
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec) throw std::runtime_error("delete error: " + ec.message());
}

void CollectionStore::ensureGitignore() const {
    fs::path gi = fs::path(root_) / ".gitignore";
    std::string content;
    fsutil::readFile(gi.string(), content);
    auto ensureLine = [&](const std::string& line) {
        if (content.find(line) == std::string::npos) {
            if (!content.empty() && content.back() != '\n') content += '\n';
            content += line + "\n";
        }
    };
    ensureLine(".session/");
    ensureLine(".secrets/");
    fsutil::writeFileAtomic(gi.string(), content);
}

} // namespace core
