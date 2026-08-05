#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>

#include <variant>

#include <nlohmann/json.hpp>                                        // _uiBodyDrafts merge/extract (infra-only)

#include "infra/serialization/json_codec.hpp"                       // parseGuarded (depth guard) for lightweight metadata reads
#include "core/domain/request/request_defaults.hpp"
#include "core/infra/persistence/request_naming.hpp"
#include "core/infra/persistence/order_key.hpp"
#include "core/infra/persistence/stores.hpp"
#include "infra/platform/fs_util.hpp"
#include "infra/serialization/request_json_mapper.hpp" // NATIVE JSON <-> domain RequestModel (REFACTOR_SPEC D)

namespace fs = std::filesystem;

namespace core {

namespace {

// Request (de)serialization is NATIVE domain: on-disk JSON <-> domain RequestModel via request_json_mapper.
// (json_codec is kept only for parseGuarded — the lightweight metadata reads in scanLevel/requestIdFromFile.)
core::domain::RequestModel requestFromText(const std::string &txt) {
  static const core::infra::RequestJsonMapper mapper;
  auto m = mapper.fromJson(txt);
  if (!m) throw std::runtime_error("parse request: " + m.error().message);
  return m.take();
}
std::string requestToText(const core::domain::RequestModel &m) {
  static const core::infra::RequestJsonMapper mapper;
  return mapper.toJson(m);
}

// Merge UI-only per-mode body drafts into a request JSON text under "_uiBodyDrafts" (the domain mapper
// rebuilds the object fresh, so this key must be added AFTER toJson). Empty drafts -> text unchanged.
std::string injectBodyDrafts(const std::string &reqJson,
                             const std::map<std::string, std::string> &drafts) {
  if (drafts.empty()) return reqJson;
  nlohmann::json j = nlohmann::json::parse(reqJson, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return reqJson;
  nlohmann::json d = nlohmann::json::object();
  for (const auto &kv : drafts) d[kv.first] = kv.second;
  j["_uiBodyDrafts"] = std::move(d);
  return j.dump(2);
}

// --- domain helpers for the filename-encoding + immutable-update the store needs ---
// HTTP method string for the filename (empty for non-HTTP — gRPC/WS/GraphQL have no method in the name).
std::string httpMethodOf(const core::domain::RequestModel &m) {
  if (m.type() != core::domain::RequestType::Http) return {};
  return core::domain::toString(std::get<core::domain::HttpRequest>(m.payload()).method());
}
// Immutable update: same payload/seq/config, new id + name (domain RequestModel has no setters).
core::domain::RequestModel withIdName(const core::domain::RequestModel &m, std::string id, std::string name) {
  return core::domain::RequestModel::create(core::domain::RequestId(std::move(id)), std::move(name), m.seq(),
                                            m.config(), m.payload())
      .take();
}
// Special dirs that are NOT shown in the request tree.
bool isReservedDir(const std::string &name) {
  return name == ".session" || name == ".secrets" || name == ".git" ||
         name == "environments";
}
// Collection/folder-level config file -> not a request.
bool isConfigFile(const std::string &name) {
  return name == "collection.json" || name == "folder.json";
}
bool isHidden(const std::string &name) {
  return !name.empty() && name[0] == '.';
}

// Generate a lightweight random id: base36 LOWERCASE, 12 chars, NO '_' (to
// embed in FILENAME — §2A). RNG seeded ONCE then advances on each call -> no
// collisions even on back-to-back calls.
std::string genId() {
  static std::mutex mu;
  static std::mt19937_64 rng([] {
    std::random_device rd;
    uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    s ^= static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return s;
  }());
  static const char *alphabet =
      "0123456789abcdefghijklmnopqrstuvwxyz"; // base36, [a-z0-9]
  std::lock_guard<std::mutex> lk(mu);
  std::string s;
  for (int i = 0; i < 12; ++i)
    s += alphabet[rng() % 36];
  return s;
}

// ---- Ordering prefix helpers (order_key.hpp) ----------------------------------------------------
// Order lives in the filename prefix "<key>+..." -> tree sorts straight off readdir (zero-read).

// Order key of a directory entry, "" when it has none.
std::string orderOf(const std::string &filename) {
  return splitOrderPrefix(filename).order;
}

// Every valid order key in `dir`, ascending. One readdir, no content reads.
std::vector<std::string> sortedOrderKeys(const fs::path &dir) {
  std::vector<std::string> keys;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    if (e.is_symlink()) continue;
    std::string fname = e.path().filename().string();
    if (isHidden(fname) || (e.is_directory() && isReservedDir(fname))) continue;
    if (e.is_regular_file() && (e.path().extension() != ".json" || isConfigFile(fname))) continue;
    std::string k = orderOf(fname);
    if (!k.empty()) keys.push_back(std::move(k));
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

// Key that places an entry last in `dir`.
std::string keyForEnd(const fs::path &dir) {
  auto keys = sortedOrderKeys(dir);
  return orderkey::between(keys.empty() ? std::string() : keys.back(), std::string());
}

// True if `dir` already holds an entry whose name matches `name` ignoring case (APFS is
// case-insensitive, so two keys differing only in case would collide on disk). `self` is skipped.
bool clashesIgnoringCase(const fs::path &dir, const std::string &name, const std::string &self) {
  auto lower = [](std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
  };
  std::string want = lower(name);
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    std::string fname = e.path().filename().string();
    if (fname == self) continue;
    if (lower(fname) == want) return true;
  }
  return false;
}

// Pick a key in (prev, next) whose resulting filename does not collide case-insensitively.
// Each retry squeezes the key further down, which changes its characters -> converges immediately
// in practice (the key space is unbounded).
std::string pickKeyBetween(const fs::path &dir, const std::string &prev, const std::string &next,
                           const std::string &rest, const std::string &self) {
  std::string key = orderkey::between(prev, next);
  for (int i = 0; i < 8; ++i) {
    if (!clashesIgnoringCase(dir, withOrderPrefix(key, rest), self)) return key;
    key = orderkey::between(prev, key);
  }
  throw std::runtime_error("could not find a non-clashing order key in: " + dir.string());
}

std::string uniquePath(const fs::path &dir, const std::string &slug,
                       const std::string &ext) {
  fs::path cand = dir / (slug + ext);
  if (!fs::exists(cand))
    return cand.string();
  for (int i = 2; i < 10000; ++i) {
    fs::path c = dir / (slug + "-" + std::to_string(i) + ext);
    if (!fs::exists(c))
      return c.string();
  }
  throw std::runtime_error("could not find a unique filename for: " + slug);
}

// Find a unique filename per the encoded grammar ([order+]<id>_type_..., id first).
// Unique ids almost never collide; still keep a slug-suffix loop to be safe.
// Returns FILENAME (no directory).
std::string uniqueEncodedName(const fs::path &dir, const std::string &id,
                              RequestType type, const std::string &method,
                              const std::string &displayName,
                              const std::string &order = "") {
  std::string first = encodeRequestFilename(id, type, method, displayName, order);
  if (!fs::exists(dir / first))
    return first;
  for (int i = 2; i < 10000; ++i) {
    std::string cand = encodeRequestFilename(
        id, type, method, displayName + " " + std::to_string(i), order);
    if (!fs::exists(dir / cand))
      return cand;
  }
  throw std::runtime_error("could not find a unique filename for: " +
                           displayName);
}

// Recursively collect the relPaths of every non-hidden request .json file under
// `rel` (folders pruned exactly like the tree scan: reserved/hidden dirs and
// config/hidden files are skipped). Collecting everything up front (rather than
// acting during iteration) makes it safe for callers that rename files.
void collectRequestFiles(const std::string &root, const std::string &rel,
                         std::vector<std::string> &out) {
  fs::path dir = fs::path(fsutil::join(root, rel));
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    if (e.is_symlink())
      continue;
    std::string fname = e.path().filename().string();
    std::string childRel = rel.empty() ? fname : rel + "/" + fname;
    if (e.is_directory()) {
      if (isReservedDir(fname) || isHidden(fname))
        continue;
      collectRequestFiles(root, childRel, out);
    } else if (e.is_regular_file()) {
      if (e.path().extension() != ".json" || isConfigFile(fname) ||
          isHidden(fname))
        continue;
      out.push_back(childRel);
    }
  }
}

// Metadata leaf from the FILENAME only — never reads content (§2). A file the app did not write
// (bad grammar) still shows up, under its raw filename.
TreeNode buildRequestLeaf(const fs::path &fullPath,
                          const std::string &relPath) {
  TreeNode leaf;
  leaf.isFolder = false;
  leaf.relPath = relPath;

  ParsedRequestName p = parseRequestFilename(fullPath.filename().string());
  if (!p.ok) {
    leaf.name = fullPath.stem().string();
    return leaf;
  }
  leaf.requestType = p.type;
  leaf.name = normalizeDisplayName(p.slug);
  if (p.type == RequestType::Http) {
    std::string m = p.method;
    for (auto &c : m)
      c = static_cast<char>(std::toupper((unsigned char)c));
    leaf.methodOrType = m; // badge HTTP method (GET/POST...)
  } else {
    leaf.methodOrType.clear(); // gRPC: UI does not show a method type
  }
  leaf.id = p.id;
  return leaf;
}

} // namespace

CollectionStore::CollectionStore(std::string root) : root_(std::move(root)) {}
void CollectionStore::setRequestDefaults(long long timeoutMs, bool verifyTls) {
  if (timeoutMs > 0) defaultTimeoutMs_ = timeoutMs;
  defaultVerifyTls_ = verifyTls;
}
void CollectionStore::setRoot(std::string root) {
  root_ = std::move(root);
  invalidateIdIndex();
}

// Scan one level — metadata-only. Subfolders kept folded (empty children). Does
// NOT read content to render (only the buildRequestLeaf fallback when a
// filename violates the grammar). §3.
std::vector<TreeNode>
CollectionStore::scanLevel(const std::string &dirRelPath) const {
  std::vector<TreeNode> out;
  fs::path dir = fs::path(fsutil::join(root_, dirRelPath));
  std::error_code ec;
  // Cache isDir + name once — comparator runs O(n log n), filename().string() allocs per call.
  struct Entry {
    fs::directory_entry ent;
    bool isDir;
    std::string name;
    std::string order;    // fractional-index prefix ("" = not ordered yet)
    std::string sortName; // display name, used only for unordered entries
  };
  std::vector<Entry> entries;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    // Do not follow symlinks (avoid recursion cycles — §10).
    if (e.is_symlink())
      continue;
    Entry en{e, e.is_directory(), e.path().filename().string(), "", ""};
    SplitOrder so = splitOrderPrefix(en.name);
    en.order = so.order;
    // Foreign entries (no key) fall back to DISPLAY name — the raw filename leads with a random id.
    en.sortName = en.isDir ? so.rest : normalizeDisplayName(parseRequestFilename(so.rest).slug);
    if (en.sortName.empty()) en.sortName = so.rest;
    entries.push_back(std::move(en));
  }
  // Keyed entries sort by key; folders and requests share ONE sequence (free interleaving).
  // Keyless = foreign file -> first, so it is visible and new requests still land last.
  std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
    bool ka = !a.order.empty(), kb = !b.order.empty();
    if (ka != kb)
      return !ka; // keyless first
    if (!ka)
      return a.sortName < b.sortName;
    return a.order < b.order;
  });
  for (const auto &en : entries) {
    const std::string &fname = en.name;
    std::string childRel =
        dirRelPath.empty() ? fname : dirRelPath + "/" + fname;
    if (en.isDir) {
      if (isReservedDir(fname) || isHidden(fname))
        continue;
      TreeNode folder;
      folder.isFolder = true;
      folder.relPath = childRel;
      folder.name = splitOrderPrefix(fname).rest; // strip the order prefix (NO de-slug)
      out.push_back(std::move(folder)); // empty children -> lazy expand later
    } else if (en.ent.is_regular_file()) {
      if (en.ent.path().extension() != ".json")
        continue;
      if (isConfigFile(fname) || isHidden(fname))
        continue;
      out.push_back(buildRequestLeaf(en.ent.path(), childRel));
    }
  }
  return out;
}

TreeNode CollectionStore::scanTree() const {
  std::function<TreeNode(const std::string &)> walk =
      [&](const std::string &rel) -> TreeNode {
    TreeNode node;
    node.isFolder = true;
    node.relPath = rel;
    node.name = rel.empty()
                    ? fs::path(root_).filename().string()
                    : splitOrderPrefix(fs::path(rel).filename().string()).rest; // strip order prefix
    for (auto &child : scanLevel(rel)) {
      if (child.isFolder)
        node.children.push_back(walk(child.relPath));
      else
        node.children.push_back(std::move(child));
    }
    return node;
  };
  return walk("");
}

core::domain::RequestModel CollectionStore::loadRequest(const std::string &relPath) const {
  std::string txt;
  if (!fsutil::readFile(fsutil::join(root_, relPath), txt))
    throw std::runtime_error("cannot read request: " + relPath);
  core::domain::RequestModel m = requestFromText(txt);
  // id must be filename-safe. Pure READ: no write here, saveRequest owns rewrites.
  if (!isValidFileId(m.id().get()))
    m = withIdName(m, genId(), m.name());
  return m;
}

// Build idIndex_ in one pass — ids come FROM THE FILENAME (zero-read).
void CollectionStore::buildIdIndexLocked() const {
  idIndex_.clear();
  std::vector<std::string> files;
  collectRequestFiles(root_, "", files);
  for (const auto &childRel : files) {
    std::string id = parseRequestFilename(fs::path(childRel).filename().string()).id;
    if (!id.empty())
      idIndex_.emplace(id, childRel); // first id wins (stable)
  }
  idIndexBuilt_ = true;
}

void CollectionStore::invalidateIdIndex() const {
  std::lock_guard<std::mutex> lk(idMu_);
  idIndexBuilt_ = false;
}

std::string CollectionStore::findRelPathById(const std::string &id) const {
  if (id.empty())
    return "";
  std::lock_guard<std::mutex> lk(idMu_);
  if (!idIndexBuilt_)
    buildIdIndexLocked();
  auto it = idIndex_.find(id);
  if (it == idIndex_.end())
    return "";
  // Validate: does the file still exist at the cached path? (guard against
  // changes OUTSIDE the app)
  // -> if gone, rebuild once and re-look up; avoid returning a "ghost" path.
  if (fs::exists(fs::path(fsutil::join(root_, it->second))))
    return it->second;
  buildIdIndexLocked();
  auto it2 = idIndex_.find(id);
  return it2 != idIndex_.end() ? it2->second : "";
}

std::map<std::string, std::string> CollectionStore::loadBodyDrafts(const std::string &relPath) const {
  std::map<std::string, std::string> out;
  std::string txt;
  if (!fsutil::readFile(fsutil::join(root_, relPath), txt)) return out;
  nlohmann::json j = nlohmann::json::parse(txt, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return out;
  auto it = j.find("_uiBodyDrafts");
  if (it == j.end() || !it->is_object()) return out;
  for (auto &el : it->items())
    if (el.value().is_string()) out[el.key()] = el.value().get<std::string>();
  return out;
}

// 2-arg overload: preserve any UI body drafts already on disk (non-editor callers don't touch them).
std::string CollectionStore::saveRequest(const std::string &relPath,
                                         const core::domain::RequestModel &m) const {
  return saveRequest(relPath, m, loadBodyDrafts(relPath));
}

std::string CollectionStore::saveRequest(const std::string &relPath, const core::domain::RequestModel &m,
                                         const std::map<std::string, std::string> &bodyDrafts) const {
  invalidateIdIndex(); // may rename file (relPath changes) -> idIndex_ must be
                       // rebuilt
  fs::path cur = fs::path(fsutil::join(root_, relPath));
  fs::path dir = cur.parent_path();
  std::string method = httpMethodOf(m);
  // Write content first (source of truth), then sync the filename = derived
  // cache (§4). UI body drafts ride along under "_uiBodyDrafts" (domain ignores them).
  fsutil::writeFileAtomic(cur.string(), injectBodyDrafts(requestToText(m), bodyDrafts));
  std::string order = orderOf(cur.filename().string()); // renaming must not drop the sibling order
  std::string desired = encodeRequestFilename(m.id().get(), m.type(), method, m.name(), order);
  if (cur.filename().string() == desired)
    return relPath; // name already matches -> done
  std::string newName = uniqueEncodedName(dir, m.id().get(), m.type(), method, m.name(), order);
  fs::path dst = dir / newName;
  std::error_code ec;
  fs::rename(cur, dst, ec); // git detects rename via unchanged content
  if (ec)
    return relPath; // rename failed -> keep old path (content already written)
  return fs::relative(dst, fs::path(root_)).generic_string();
}

std::string CollectionStore::createRequest(const std::string &folderRel,
                                           RequestType type,
                                           const std::string &name) const {
  invalidateIdIndex();
  fs::path dir = fs::path(fsutil::join(root_, folderRel));
  fs::create_directories(dir);
  // Build a fresh domain request of `type` with the domain default payload (HTTP ships the common headers
  // OFF-by-default except User-Agent=deed; gRPC reflection/unary; GraphQL starter doc). NEW requests only —
  // existing requests (load/save) and imports are never auto-modified.
  std::string id = genId();
  // New-request default config: timeout + TLS verify from .env (via setRequestDefaults), not hardcoded.
  core::domain::RequestConfig cfg{core::domain::Timeout::fromMillis(defaultTimeoutMs_).take(),
                                  defaultVerifyTls_};
  core::domain::RequestModel m = core::domain::RequestModel::create(
      core::domain::RequestId(id), name, 0, cfg, core::domain::defaultPayloadFor(type))
      .take();
  std::string method = httpMethodOf(m);
  // New requests go to the BOTTOM of their level (one readdir for the current max key).
  fs::path full = dir / uniqueEncodedName(dir, id, type, method, name, keyForEnd(dir));
  fsutil::writeFileAtomic(full.string(), requestToText(m));
  return fs::relative(full, fs::path(root_)).generic_string();
}

std::string
CollectionStore::createRequestFromModel(const std::string &folderRel,
                                        core::domain::RequestModel m,
                                        const std::string &name) const {
  invalidateIdIndex();
  fs::path dir = fs::path(fsutil::join(root_, folderRel));
  fs::create_directories(dir);
  std::string id = genId(); // new id, independent of the import source
  m = withIdName(m, id, name);
  std::string method = httpMethodOf(m);
  // Imported requests land at the BOTTOM, same as freshly created ones.
  fs::path full = dir / uniqueEncodedName(dir, id, m.type(), method, name, keyForEnd(dir));
  fsutil::writeFileAtomic(full.string(), requestToText(m));
  return fs::relative(full, fs::path(root_)).generic_string();
}

std::string CollectionStore::createFolder(const std::string &parentRel,
                                          const std::string &name) const {
  invalidateIdIndex();
  std::string slug = fsutil::slugify(name);
  fs::path parent = fs::path(fsutil::join(root_, parentRel));
  fs::create_directories(parent);
  // New folders go to the BOTTOM of their level, like new requests. pickKeyBetween also guards the
  // case-insensitive clash two folders with the same slug could otherwise hit on APFS.
  auto keys = sortedOrderKeys(parent);
  std::string key = pickKeyBetween(parent, keys.empty() ? std::string() : keys.back(),
                                   std::string(), slug, "");
  fs::path dir = parent / withOrderPrefix(key, slug);
  fs::create_directories(dir);
  return fs::relative(dir, fs::path(root_)).generic_string();
}

std::string CollectionStore::rename(const std::string &relPath,
                                    const std::string &newName) const {
  invalidateIdIndex();
  fs::path src = fs::path(fsutil::join(root_, relPath));
  if (!fs::exists(src))
    throw std::runtime_error("does not exist: " + relPath);
  if (fs::is_directory(src)) {
    // Keep the order prefix — renaming a folder must not move it in the level.
    std::string order = orderOf(src.filename().string());
    fs::path dst = src.parent_path() / withOrderPrefix(order, fsutil::slugify(newName));
    fs::rename(src, dst);
    return fs::relative(dst, fs::path(root_)).generic_string();
  }
  // request: update the name field + rename the file (KEEP old id, change only
  // the slug). §2A.
  core::domain::RequestModel m = loadRequest(relPath);
  m = withIdName(m, m.id().get(), newName); // keep id, change name (immutable update)
  std::string method = httpMethodOf(m);
  std::string text = injectBodyDrafts(requestToText(m), loadBodyDrafts(relPath));
  std::string order = orderOf(src.filename().string()); // keep the sibling order across a rename
  std::string desired = encodeRequestFilename(m.id().get(), m.type(), method, newName, order);
  if (src.filename().string() == desired) {
    fsutil::writeFileAtomic(src.string(), text); // name already matches -> no rename
    return relPath;
  }
  fs::path newFull =
      src.parent_path() /
      uniqueEncodedName(src.parent_path(), m.id().get(), m.type(), method, newName, order);
  fsutil::writeFileAtomic(newFull.string(), text);
  fs::remove(src);
  return fs::relative(newFull, fs::path(root_)).generic_string();
}

std::string CollectionStore::duplicate(const std::string &relPath) const {
  invalidateIdIndex();
  fs::path src = fs::path(fsutil::join(root_, relPath));
  if (!fs::exists(src))
    throw std::runtime_error("does not exist: " + relPath);
  // The copy sits immediately BELOW the original: a key between it and its next sibling.
  std::string origKey = orderOf(src.filename().string());
  auto keyAfterOriginal = [&](const std::string &rest) {
    auto keys = sortedOrderKeys(src.parent_path());
    auto it = std::upper_bound(keys.begin(), keys.end(), origKey);
    std::string next = it == keys.end() ? std::string() : *it;
    return pickKeyBetween(src.parent_path(), origKey, next, rest, "");
  };
  if (fs::is_directory(src)) {
    std::string slug = splitOrderPrefix(src.filename().string()).rest + "-copy";
    fs::path dst = src.parent_path() / withOrderPrefix(keyAfterOriginal(slug), slug);
    std::error_code ec;
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    if (ec)
      throw std::runtime_error("duplicate folder error: " + ec.message());
    return fs::relative(dst, fs::path(root_)).generic_string();
  }
  core::domain::RequestModel m = loadRequest(relPath);
  m = withIdName(m, genId(), m.name() + " copy"); // new id to avoid collisions
  std::string method = httpMethodOf(m);
  std::string dupKey =
      origKey.empty() ? std::string() // original not ordered yet -> stay unordered too
                      : keyAfterOriginal(encodeRequestFilename(m.id().get(), m.type(), method, m.name()));
  fs::path newFull =
      src.parent_path() /
      uniqueEncodedName(src.parent_path(), m.id().get(), m.type(), method, m.name(), dupKey);
  fsutil::writeFileAtomic(newFull.string(), injectBodyDrafts(requestToText(m), loadBodyDrafts(relPath)));
  return fs::relative(newFull, fs::path(root_)).generic_string();
}

std::string CollectionStore::reorder(const std::string &relPath,
                                     const std::string &destFolderRel, int index) const {
  fs::path src = fs::path(fsutil::join(root_, relPath));
  if (!fs::exists(src))
    throw std::runtime_error("does not exist: " + relPath);
  fs::path destDir = fs::path(fsutil::join(root_, destFolderRel));
  fs::create_directories(destDir);
  // Block moving a folder into itself / its descendants (same guard as move()).
  if (fs::is_directory(src)) {
    auto s = fs::weakly_canonical(src);
    auto d = fs::weakly_canonical(destDir);
    auto mismatch = std::mismatch(s.begin(), s.end(), d.begin(), d.end());
    if (mismatch.first == s.end())
      throw std::runtime_error("cannot move a folder into itself");
  }
  invalidateIdIndex();

  const std::string srcName = src.filename().string();
  bool sameLevel = fs::equivalent(src.parent_path(), destDir);
  // `index` is a slot in the level AS DISPLAYED — the dragged row still counted. Drop the entry from
  // the neighbour list and, when it sat ABOVE the target slot, shift the slot down by one; otherwise
  // dragging downwards would always land one position too far.
  std::vector<std::string> keys;
  int selfAt = -1;
  for (const auto &n : scanLevel(destFolderRel)) {
    std::string fname = fs::path(n.relPath).filename().string();
    if (sameLevel && fname == srcName) {
      selfAt = static_cast<int>(keys.size());
      continue;
    }
    std::string k = orderOf(fname);
    if (!k.empty())
      keys.push_back(std::move(k));
  }
  int at = index < 0 ? 0 : index;
  if (selfAt >= 0 && at > selfAt)
    --at;
  if (at > static_cast<int>(keys.size()))
    at = static_cast<int>(keys.size());
  std::string prev = at > 0 ? keys[static_cast<std::size_t>(at) - 1] : std::string();
  std::string next = at < static_cast<int>(keys.size()) ? keys[static_cast<std::size_t>(at)] : std::string();

  std::string rest = splitOrderPrefix(srcName).rest;
  std::string key = pickKeyBetween(destDir, prev, next, rest, sameLevel ? srcName : std::string());
  fs::path dest = destDir / withOrderPrefix(key, rest);
  if (dest == src)
    return relPath;
  fs::rename(src, dest);
  return fs::relative(dest, fs::path(root_)).generic_string();
}

std::string CollectionStore::move(const std::string &relPath,
                                  const std::string &destFolderRel) const {
  invalidateIdIndex();
  fs::path src = fs::path(fsutil::join(root_, relPath));
  if (!fs::exists(src))
    throw std::runtime_error("does not exist: " + relPath);
  fs::path destDir = fs::path(fsutil::join(root_, destFolderRel));
  fs::create_directories(destDir);

  // Block moving a folder into itself / its descendants.
  if (fs::is_directory(src)) {
    auto s = fs::weakly_canonical(src);
    auto d = fs::weakly_canonical(destDir);
    auto mismatch = std::mismatch(s.begin(), s.end(), d.begin(), d.end());
    if (mismatch.first == s.end())
      throw std::runtime_error("cannot move a folder into itself");
  }
  // Do nothing if already in the right folder.
  if (fs::equivalent(src.parent_path(), destDir))
    return relPath;

  // Re-key for the destination level: the old key belongs to the source level's sequence and could
  // even duplicate one already there. Landing at the bottom matches "dropped into this folder".
  std::string rest = splitOrderPrefix(src.filename().string()).rest;
  auto keys = sortedOrderKeys(destDir);
  std::string newKey =
      pickKeyBetween(destDir, keys.empty() ? std::string() : keys.back(), std::string(), rest, "");
  std::string wanted = withOrderPrefix(newKey, rest);
  fs::path dest = destDir / wanted;
  if (!fs::is_directory(src) && fs::exists(dest)) {   // same id in the target -> keep the -2 fallback
    fs::path w(wanted);
    dest = fs::path(uniquePath(destDir, w.stem().string(),
                               w.has_extension() ? w.extension().string() : ""));
  }
  fs::rename(src, dest);
  return fs::relative(dest, fs::path(root_)).generic_string();
}

void CollectionStore::remove(const std::string &relPath) const {
  invalidateIdIndex();
  fs::path p = fs::path(fsutil::join(root_, relPath));
  std::error_code ec;
  fs::remove_all(p, ec);
  if (ec)
    throw std::runtime_error("delete error: " + ec.message());
}

void CollectionStore::ensureGitignore() const {
  fs::path gi = fs::path(root_) / ".gitignore";
  std::string content;
  fsutil::readFile(gi.string(), content);
  auto ensureLine = [&](const std::string &line) {
    if (content.find(line) == std::string::npos) {
      if (!content.empty() && content.back() != '\n')
        content += '\n';
      content += line + "\n";
    }
  };
  ensureLine(".session/");
  ensureLine(".secrets/");
  fsutil::writeFileAtomic(gi.string(), content);
}

} // namespace core
