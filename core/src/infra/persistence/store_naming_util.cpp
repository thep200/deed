#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "infra/persistence/store_naming_util.hpp"
#include "core/infra/persistence/order_key.hpp"
#include "core/infra/persistence/request_naming.hpp"

namespace fs = std::filesystem;

namespace core::store_detail {

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

// base36 lowercase, 12 chars, no '_' (embedded in the filename grammar); RNG seeded once so
// back-to-back calls cannot collide.
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

// Order lives in the filename prefix "<key>+..." -> the tree sorts straight off readdir (zero-read).
std::string orderOf(const std::string &filename) {
  return splitOrderPrefix(filename).order;
}

// One readdir, no content reads.
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

std::string keyForEnd(const fs::path &dir) {
  auto keys = sortedOrderKeys(dir);
  return orderkey::between(keys.empty() ? std::string() : keys.back(), std::string());
}

// APFS is case-insensitive — two names differing only in case collide on disk. `self` is skipped.
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

// Each retry squeezes the key further down, changing its characters -> converges immediately in practice.
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

// Returns a filename only (no directory); ids almost never collide but the slug-suffix loop stays as a guard.
std::string uniqueEncodedName(const fs::path &dir, const std::string &id,
                              RequestType type, const std::string &method,
                              const std::string &displayName,
                              const std::string &order) {
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

} // namespace core::store_detail
