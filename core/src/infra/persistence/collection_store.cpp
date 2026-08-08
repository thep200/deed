#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "infra/serialization/json_codec.hpp"
#include "core/infra/persistence/request_naming.hpp"
#include "core/infra/persistence/stores.hpp"
#include "infra/persistence/store_naming_util.hpp"
#include "infra/platform/fs_util.hpp"
#include "infra/serialization/request_json_mapper.hpp"

namespace fs = std::filesystem;

namespace core {

using namespace store_detail;

namespace {

core::domain::RequestModel requestFromText(const std::string &txt) {
  static const core::infra::RequestJsonMapper mapper;
  auto m = mapper.fromJson(txt);
  if (!m) throw std::runtime_error("parse request: " + m.error().message);
  return m.take();
}

// Collects up front (not while iterating) so callers can rename files safely; pruning matches the tree scan.
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

// Filename-only, never reads content; a file with bad name grammar still shows under its raw filename.
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

// Metadata-only: subfolders stay folded (empty children); content is never read to render.
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
    // Do not follow symlinks (recursion cycles).
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
  // Folders and requests share ONE key sequence; keyless (foreign) first, so they stay visible and new requests land last.
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
  // Guard against changes outside the app: cached path gone -> rebuild once and re-look up.
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

} // namespace core
