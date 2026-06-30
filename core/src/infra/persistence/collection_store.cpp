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
#include "core/infra/persistence/request_naming.hpp"
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
// The survivor view enum used by request-filename encoding.
RequestType toViewType(core::domain::RequestType t) {
  switch (t) {
  case core::domain::RequestType::Grpc: return RequestType::Grpc;
  case core::domain::RequestType::WebSocket: return RequestType::WebSocket;
  case core::domain::RequestType::GraphQl: return RequestType::GraphQL;
  default: return RequestType::Http;
  }
}
core::domain::RequestType toDomainType(RequestType t) {
  switch (t) {
  case RequestType::Grpc: return core::domain::RequestType::Grpc;
  case RequestType::WebSocket: return core::domain::RequestType::WebSocket;
  case RequestType::GraphQL: return core::domain::RequestType::GraphQl;
  default: return core::domain::RequestType::Http;
  }
}
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
// Default payload for a freshly created request of `type` (mirrors the legacy createRequest defaults).
// NB: fully-qualified core::domain:: — a `using namespace core::domain` would clash with the legacy core::
// types (Auth/WsSendKind/GrpcRequest/...) still reachable here via stores.hpp -> types.hpp.
core::domain::RequestModel::Payload defaultPayloadForCreate(core::domain::RequestType type) {
  namespace cd = core::domain;
  switch (type) {
  case cd::RequestType::WebSocket:
    return cd::WebSocketRequest::create(
               {cd::Url::create("").take(), {}, {}, cd::Auth::none(), {}, cd::WsSendKind::Text})
        .take();
  case cd::RequestType::GraphQl: {
    cd::GraphQlOperation op;
    op.query = "query {\n  \n}"; // starter document
    return cd::GraphQlRequest::create(
               {cd::Url::create("").take(), op, {}, cd::Auth::none(), cd::GqlSubTransport::Http, ""})
        .take();
  }
  case cd::RequestType::Grpc: {
    cd::GrpcRequest::Parts gp; // reflection + unary + empty target are the Parts defaults
    gp.message = cd::JsonText::of("{}");
    return cd::GrpcRequest::create(std::move(gp)).take();
  }
  default: {
    // HTTP: GET + the common default headers as OFF-by-default hints (User-Agent on), no body.
    std::vector<cd::Header> hdrs;
    hdrs.push_back(cd::Header::create("Content-Type", "application/json", false).take());
    hdrs.push_back(cd::Header::create("Accept", "*/*", false).take());
    hdrs.push_back(cd::Header::create("User-Agent", "deed", true).take());
    hdrs.push_back(cd::Header::create("Accept-Encoding", "gzip, deflate, br", false).take());
    hdrs.push_back(cd::Header::create("Connection", "keep-alive", false).take());
    return cd::HttpRequest::create({cd::HttpMethod::Get, cd::Url::create("").take(), {}, {},
                                    cd::HeaderList(std::move(hdrs)), cd::Body::none(), cd::Auth::none()})
        .take();
  }
  }
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

// id usable in a filename? (legacy "req_..." contains '_' -> invalid -> must
// regenerate on migrate).
std::string ensureFileId(const std::string &id) {
  return isValidFileId(id) ? id : genId();
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

// Find a unique filename per the encoded grammar (<id>_type_..., id first).
// Unique ids almost never collide; still keep a slug-suffix loop to be safe.
// Returns FILENAME (no directory).
std::string uniqueEncodedName(const fs::path &dir, const std::string &id,
                              RequestType type, const std::string &method,
                              const std::string &displayName) {
  std::string first = encodeRequestFilename(id, type, method, displayName);
  if (!fs::exists(dir / first))
    return first;
  for (int i = 2; i < 10000; ++i) {
    std::string cand = encodeRequestFilename(
        id, type, method, displayName + " " + std::to_string(i));
    if (!fs::exists(dir / cand))
      return cand;
  }
  throw std::runtime_error("could not find a unique filename for: " +
                           displayName);
}

// Read one request file's content `id` (empty if unreadable / not JSON / no
// id).
std::string idFromContent(const fs::path &fullPath) {
  std::string txt;
  if (fsutil::readFile(fullPath.string(), txt)) {
    try {
      return codec::parseGuarded(txt).value("id", std::string());
    } catch (...) {
    }
  }
  return "";
}

// id preferred FROM FILENAME (zero-read); read content only for a legacy file
// with no id in its name.
std::string requestIdFromFile(const fs::path &fullPath) {
  ParsedRequestName p = parseRequestFilename(fullPath.filename().string());
  return p.id.empty() ? idFromContent(fullPath) : p.id;
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

// Fallback when the filename violates the grammar: read the file once for the
// real type/method/name/id.
void fillLeafFromContent(TreeNode &leaf, const fs::path &fullPath) {
  leaf.name = fullPath.stem().string(); // at minimum: filename (without .json)
  std::string txt;
  if (!fsutil::readFile(fullPath.string(), txt))
    return;
  try {
    auto j = codec::parseGuarded(txt);
    if (j.contains("name") && j["name"].is_string())
      leaf.name = j["name"].get<std::string>();
    leaf.id = j.value("id", std::string());
    std::string t = j.value("type", "http");
    parseRequestType(t, leaf.requestType);
    if (leaf.requestType == RequestType::Http)
      leaf.methodOrType =
          j.value("http", codec::json::object()).value("method", "GET");
    else
      leaf.methodOrType.clear();
  } catch (...) { /* bad file -> still show the filename */
  }
}

// Build metadata leaf for one request file — does NOT read content when the
// filename is valid (§2). Only falls back to a single read if the filename
// violates the grammar (§5).
TreeNode buildRequestLeaf(const fs::path &fullPath,
                          const std::string &relPath) {
  TreeNode leaf;
  leaf.isFolder = false;
  leaf.relPath = relPath;

  ParsedRequestName p = parseRequestFilename(fullPath.filename().string());
  if (!p.ok) { // grammar violated -> fall back to a single content read
    fillLeafFromContent(leaf, fullPath);
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
  // id READ FROM FILENAME (zero-read); only an OLD file with no id in its name
  // needs one content read.
  leaf.id = p.id.empty() ? idFromContent(fullPath) : p.id;
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
  std::vector<fs::directory_entry> entries;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    // Do not follow symlinks (avoid recursion cycles — §10).
    if (e.is_symlink())
      continue;
    entries.push_back(e);
  }
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    if (a.is_directory() != b.is_directory())
      return a.is_directory(); // folders first
    return a.path().filename().string() < b.path().filename().string();
  });
  for (const auto &e : entries) {
    std::string fname = e.path().filename().string();
    std::string childRel =
        dirRelPath.empty() ? fname : dirRelPath + "/" + fname;
    if (e.is_directory()) {
      if (isReservedDir(fname) || isHidden(fname))
        continue;
      TreeNode folder;
      folder.isFolder = true;
      folder.relPath = childRel;
      folder.name = fname;              // folder: directory name (NO de-slug)
      out.push_back(std::move(folder)); // empty children -> lazy expand later
    } else if (e.is_regular_file()) {
      if (e.path().extension() != ".json")
        continue;
      if (isConfigFile(fname) || isHidden(fname))
        continue;
      out.push_back(buildRequestLeaf(e.path(), childRel));
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
    node.name = rel.empty() ? fs::path(root_).filename().string()
                            : fs::path(rel).filename().string();
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
  // id must be valid to embed in the FILENAME ([a-z0-9], no '_'). Empty/legacy "req_..." content
  // -> do NOT write to disk here (loadRequest is a pure READ — avoid write-storm on open/browse). Prefer the
  // id FROM THE FILENAME (stable after migrateAddIdToFilenames); only generate a temp one if the filename
  // also lacks an id. Content rewrites are owned by saveRequest/migrateAddIdToFilenames.
  if (!isValidFileId(m.id().get())) {
    ParsedRequestName p = parseRequestFilename(fs::path(relPath).filename().string());
    std::string id = (p.ok && isValidFileId(p.id)) ? p.id : genId();
    m = withIdName(m, std::move(id), m.name());
  }
  return m;
}

// Build idIndex_ in one pass: id preferred FROM FILENAME (zero-read), read
// content only for legacy files.
void CollectionStore::buildIdIndexLocked() const {
  idIndex_.clear();
  std::vector<std::string> files;
  collectRequestFiles(root_, "", files);
  for (const auto &childRel : files) {
    std::string id = requestIdFromFile(fs::path(fsutil::join(root_, childRel)));
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

// Rename one legacy file to embed its id (the <id>_type_... grammar). Returns
// true if a rename happened.
bool CollectionStore::migrateOneFile(const std::string &childRel) const {
  try {
    core::domain::RequestModel m =
        loadRequest(childRel); // ensure a clean id (generate if legacy/empty)
    std::string method = httpMethodOf(m);
    fs::path src = fs::path(fsutil::join(root_, childRel));
    std::string newName =
        uniqueEncodedName(src.parent_path(), m.id().get(), toViewType(m.type()), method, m.name());
    if (newName == src.filename().string())
      return false;
    std::error_code ec;
    fs::rename(src, src.parent_path() / newName, ec);
    return !ec;
  } catch (...) {
    return false; // bad file -> skip, do not block migrating the rest
  }
}

int CollectionStore::migrateAddIdToFilenames() const {
  // Collect all request files FIRST (full traversal completes before any rename
  // — safe to mutate dirs).
  std::vector<std::string> files;
  collectRequestFiles(root_, "", files);
  int migrated = 0;
  for (const auto &childRel : files) {
    ParsedRequestName p =
        parseRequestFilename(fs::path(childRel).filename().string());
    if (p.ok && !p.id.empty())
      continue; // already has id in name -> do NOT read content
    if (migrateOneFile(childRel))
      ++migrated;
  }
  if (migrated)
    invalidateIdIndex(); // renamed files -> idIndex_ (keyed by relPath) is now
                         // stale
  return migrated;
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
  std::string desired = encodeRequestFilename(m.id().get(), toViewType(m.type()), method, m.name());
  if (cur.filename().string() == desired)
    return relPath; // name already matches -> done
  std::string newName = uniqueEncodedName(dir, m.id().get(), toViewType(m.type()), method, m.name());
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
  // Build a fresh domain request of `type` with the store's default payload (HTTP ships the common headers
  // OFF-by-default except User-Agent=deed; gRPC reflection/unary; GraphQL starter doc). NEW requests only —
  // existing requests (load/save) and imports are never auto-modified.
  std::string id = genId();
  // New-request default config: timeout + TLS verify from .env (via setRequestDefaults), not hardcoded.
  core::domain::RequestConfig cfg{core::domain::Timeout::fromMillis(defaultTimeoutMs_).take(),
                                  defaultVerifyTls_};
  core::domain::RequestModel m = core::domain::RequestModel::create(
      core::domain::RequestId(id), name, 0, cfg, defaultPayloadForCreate(toDomainType(type)))
      .take();
  std::string method = httpMethodOf(m);
  fs::path full = dir / uniqueEncodedName(dir, id, type, method, name);
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
  fs::path full = dir / uniqueEncodedName(dir, id, toViewType(m.type()), method, name);
  fsutil::writeFileAtomic(full.string(), requestToText(m));
  return fs::relative(full, fs::path(root_)).generic_string();
}

std::string CollectionStore::createFolder(const std::string &parentRel,
                                          const std::string &name) const {
  invalidateIdIndex();
  std::string slug = fsutil::slugify(name);
  fs::path dir = fs::path(fsutil::join(fsutil::join(root_, parentRel), slug));
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
    fs::path dst = src.parent_path() / fsutil::slugify(newName);
    fs::rename(src, dst);
    return fs::relative(dst, fs::path(root_)).generic_string();
  }
  // request: update the name field + rename the file (KEEP old id, change only
  // the slug). §2A.
  core::domain::RequestModel m = loadRequest(relPath);
  m = withIdName(m, m.id().get(), newName); // keep id, change name (immutable update)
  std::string method = httpMethodOf(m);
  fs::path newFull =
      src.parent_path() /
      uniqueEncodedName(src.parent_path(), m.id().get(), toViewType(m.type()), method, newName);
  fsutil::writeFileAtomic(newFull.string(), requestToText(m));
  fs::remove(src);
  return fs::relative(newFull, fs::path(root_)).generic_string();
}

std::string CollectionStore::duplicate(const std::string &relPath) const {
  invalidateIdIndex();
  fs::path src = fs::path(fsutil::join(root_, relPath));
  if (!fs::exists(src))
    throw std::runtime_error("does not exist: " + relPath);
  if (fs::is_directory(src)) {
    fs::path dst = src.parent_path() / (src.filename().string() + "-copy");
    std::error_code ec;
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    if (ec)
      throw std::runtime_error("duplicate folder error: " + ec.message());
    return fs::relative(dst, fs::path(root_)).generic_string();
  }
  core::domain::RequestModel m = loadRequest(relPath);
  m = withIdName(m, genId(), m.name() + " copy"); // new id to avoid collisions
  std::string method = httpMethodOf(m);
  fs::path newFull =
      src.parent_path() /
      uniqueEncodedName(src.parent_path(), m.id().get(), toViewType(m.type()), method, m.name());
  fsutil::writeFileAtomic(newFull.string(), requestToText(m));
  return fs::relative(newFull, fs::path(root_)).generic_string();
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

  std::string stem = src.stem().string();
  std::string ext = src.has_extension() ? src.extension().string() : "";
  fs::path dest = fs::is_directory(src)
                      ? (destDir / src.filename())
                      : fs::path(uniquePath(destDir, stem, ext));
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
