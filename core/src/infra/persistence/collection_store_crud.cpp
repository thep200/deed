#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <variant>

#include <nlohmann/json.hpp>

#include "core/domain/request/request_defaults.hpp"
#include "core/infra/persistence/order_key.hpp"
#include "core/infra/persistence/request_naming.hpp"
#include "core/infra/persistence/stores.hpp"
#include "infra/persistence/store_naming_util.hpp"
#include "infra/platform/fs_util.hpp"
#include "infra/serialization/request_json_mapper.hpp"

namespace fs = std::filesystem;

namespace core {

using namespace store_detail;

namespace {

std::string requestToText(const core::domain::RequestModel &m) {
  static const core::infra::RequestJsonMapper mapper;
  return mapper.toJson(m);
}

// The mapper rebuilds the JSON object fresh, so "_uiBodyDrafts" must be re-added AFTER toJson.
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

// Levels written before fractional indexing carry no keys at all, and scanLevel sorts keyless entries
// FIRST — so a freshly keyed entry can only ever land below them (drag up = no-op, drag down = jump to
// the bottom). Key the whole level once, in the order the user is looking at, so a display slot and a
// key slot mean the same thing. Returns the renames performed (old filename -> new filename).
std::map<std::string, std::string> backfillOrderKeys(const fs::path &dir,
                                                     const std::vector<core::TreeNode> &level) {
  std::map<std::string, std::string> renamed;
  bool anyKeyless = false;
  for (const auto &n : level)
    if (core::splitOrderPrefix(fs::path(n.relPath).filename().string()).order.empty()) {
      anyKeyless = true;
      break;
    }
  if (!anyKeyless)
    return renamed;
  std::string prev;
  for (const auto &n : level) {
    std::string name = fs::path(n.relPath).filename().string();
    std::string key = core::orderkey::between(prev, std::string());
    prev = key;
    // The part after the key is unique per entry (request id / folder slug), so no target can clash.
    std::string want = core::withOrderPrefix(key, core::splitOrderPrefix(name).rest);
    if (want == name)
      continue;
    fs::rename(dir / name, dir / want);
    renamed.emplace(name, want);
  }
  return renamed;
}

// Empty for non-HTTP — only HTTP filenames carry a method.
std::string httpMethodOf(const core::domain::RequestModel &m) {
  if (m.type() != core::domain::RequestType::Http) return {};
  return core::domain::toString(std::get<core::domain::HttpRequest>(m.payload()).method());
}

} // namespace

// 2-arg overload: preserve any UI body drafts already on disk (non-editor callers don't touch them).
std::string CollectionStore::saveRequest(const std::string &relPath,
                                         const core::domain::RequestModel &m) const {
  return saveRequest(relPath, m, loadBodyDrafts(relPath));
}

std::string CollectionStore::saveRequest(const std::string &relPath, const core::domain::RequestModel &m,
                                         const std::map<std::string, std::string> &bodyDrafts) const {
  invalidateIdIndex(); // may rename the file (relPath changes)
  fs::path cur = fs::path(fsutil::join(root_, relPath));
  fs::path dir = cur.parent_path();
  std::string method = httpMethodOf(m);
  // Content first (source of truth), then sync the filename (derived cache).
  fsutil::writeFileAtomic(cur.string(), injectBodyDrafts(requestToText(m), bodyDrafts));
  std::string order = orderOf(cur.filename().string()); // renaming must not drop the sibling order
  std::string desired = encodeRequestFilename(m.id().get(), m.type(), method, m.name(), order);
  if (cur.filename().string() == desired)
    return relPath;
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
  // Default payload applies to NEW requests only — existing requests and imports are never auto-modified.
  std::string id = genId();
  // timeout + TLS verify come from .env via setRequestDefaults, not hardcoded.
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
  core::domain::RequestModel m = loadRequest(relPath);
  m = withIdName(m, m.id().get(), newName); // keep id, change name (immutable update)
  std::string method = httpMethodOf(m);
  std::string text = injectBodyDrafts(requestToText(m), loadBodyDrafts(relPath));
  std::string order = orderOf(src.filename().string()); // keep the sibling order across a rename
  std::string desired = encodeRequestFilename(m.id().get(), m.type(), method, newName, order);
  if (src.filename().string() == desired) {
    fsutil::writeFileAtomic(src.string(), text);
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

  std::vector<TreeNode> level = scanLevel(destFolderRel);
  auto renamed = backfillOrderKeys(destDir, level);
  if (!renamed.empty()) {
    auto it = renamed.find(src.filename().string()); // the dragged entry may have been re-keyed too
    if (it != renamed.end())
      src = src.parent_path() / it->second;
    level = scanLevel(destFolderRel);
  }

  const std::string srcName = src.filename().string();
  bool sameLevel = fs::equivalent(src.parent_path(), destDir);
  // `index` is a slot as DISPLAYED (dragged row still counted): drop it from the neighbour list and
  // shift the slot down when it sat above — else dragging downwards lands one position too far.
  // Every entry is keyed by now, so slot i of the display is slot i of `keys`.
  std::vector<std::string> keys;
  int selfAt = -1;
  for (const auto &n : level) {
    std::string fname = fs::path(n.relPath).filename().string();
    if (sameLevel && fname == srcName) {
      selfAt = static_cast<int>(keys.size());
      continue;
    }
    keys.push_back(orderOf(fname));
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
  if (fs::equivalent(src.parent_path(), destDir))
    return relPath;

  // Re-key for the destination level (the old key could duplicate one there); land at the bottom.
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
