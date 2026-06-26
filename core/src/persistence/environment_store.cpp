#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "core/persistence/stores.hpp"
#include "infra/fs_util.hpp"
#include "codec/json_codec.hpp"

namespace fs = std::filesystem;

namespace core {

EnvironmentStore::EnvironmentStore(std::string root) : root_(std::move(root)) {}

void EnvironmentStore::setRoot(std::string root) {
    root_ = std::move(root);
    epoch_.fetch_add(1, std::memory_order_relaxed);   // collection changed -> vars cache must be rebuilt
}

namespace {
std::string envDir(const std::string& root) { return fsutil::join(root, "environments"); }
std::string envFile(const std::string& root, const std::string& name) {
    return fsutil::join(envDir(root), name + ".json");
}
} // namespace

std::vector<std::string> EnvironmentStore::list() const {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path dir(envDir(root_));
    if (!fs::exists(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        const auto& p = e.path();
        if (p.extension() == ".json") out.push_back(p.stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

Environment EnvironmentStore::load(const std::string& name) const {
    std::string txt;
    if (!fsutil::readFile(envFile(root_, name), txt))
        throw std::runtime_error("env not found: " + name);
    Environment e = codec::envFromJson(codec::parseGuarded(txt));
    e.name = name;   // the env name IS the filename (authoritative) — ignore any stale "name" inside the JSON
    return e;
}

void EnvironmentStore::save(const Environment& e) {
    if (e.name.empty()) throw std::runtime_error("env must have a name");
    fsutil::writeFileAtomic(envFile(root_, e.name), codec::toJson(e).dump(2));
    epoch_.fetch_add(1, std::memory_order_relaxed);
}

void EnvironmentStore::remove(const std::string& name) {
    std::error_code ec;
    fs::remove(fs::path(envFile(root_, name)), ec);
    epoch_.fetch_add(1, std::memory_order_relaxed);
}

bool EnvironmentStore::renameEnv(const std::string& oldName, const std::string& newName) {
    if (newName.empty() || oldName.empty()) return false;
    if (oldName == newName) return true;
    std::error_code ec;
    // Collides with another env name -> reject.
    if (fs::exists(fs::path(envFile(root_, newName)), ec)) return false;
    fs::path src(envFile(root_, oldName));
    if (!fs::exists(src, ec)) return false;
    // Read, rename inside content, atomically write the new file, then delete the old one.
    Environment e = load(oldName);
    e.name = newName;
    fsutil::writeFileAtomic(envFile(root_, newName), codec::toJson(e).dump(2));
    fs::remove(src, ec);
    epoch_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool EnvironmentStore::renameAlias(const std::string& oldAlias, const std::string& newAlias) {
    if (newAlias.empty() || oldAlias.empty()) return false;
    if (oldAlias == newAlias) return true;
    // Cross-env: check for collisions across ALL envs first, then write (logically atomic).
    auto names = list();
    std::vector<Environment> envs;
    envs.reserve(names.size());
    for (const auto& n : names) {
        Environment e;
        try { e = load(n); } catch (...) { continue; }
        bool hasOld = false, hasNew = false;
        for (const auto& k : e.keys) {
            if (k.key == oldAlias) hasOld = true;
            if (k.key == newAlias) hasNew = true;
        }
        if (hasOld && hasNew) return false; // would create a duplicate key -> reject
        envs.push_back(std::move(e));
    }
    bool changedAny = false;
    for (auto& e : envs) {
        bool changed = false;
        for (auto& k : e.keys) {
            if (k.key == oldAlias) { k.key = newAlias; changed = true; }
        }
        // M16: write directly (no per-env epoch bump) so a rename touching K envs does K writes but only
        // ONE epoch bump + cache invalidation at the end, instead of K.
        if (changed) {
            fsutil::writeFileAtomic(envFile(root_, e.name), codec::toJson(e).dump(2));
            changedAny = true;
        }
    }
    if (changedAny) epoch_.fetch_add(1, std::memory_order_relaxed);
    return changedAny;
}

void EnvironmentStore::migrateLegacySecrets() {
    fs::path secretsDir = fs::path(root_) / ".secrets";
    fs::path secretsFile = secretsDir / "secrets.json";
    std::error_code ec;
    if (!fs::exists(secretsDir, ec)) return; // already migrated / never had secrets -> no-op

    std::string txt;
    if (fsutil::readFile(secretsFile.string(), txt)) {
        try {
            auto j = codec::parseGuarded(txt);
            if (j.is_object()) {
                // Old layout: { "<env>": { "<key>": "<value>" } }
                for (auto it = j.begin(); it != j.end(); ++it) {
                    const std::string& envName = it.key();
                    if (!it->is_object()) continue;
                    Environment e;
                    try { e = load(envName); } catch (...) { e.name = envName; }
                    for (auto kit = it->begin(); kit != it->end(); ++kit) {
                        if (!kit->is_string()) continue;
                        const std::string& key = kit.key();
                        const std::string val = kit->get<std::string>();
                        bool found = false;
                        for (auto& ek : e.keys)
                            if (ek.key == key) { ek.value = val; found = true; break; }
                        if (!found) e.keys.push_back(EnvKey{key, val, true});
                    }
                    if (!e.name.empty()) save(e);
                }
            }
        } catch (...) { /* corrupt .secrets -> skip, still allow opening the app (SPEC edge case) */ }
    }
    // Delete .secrets/ -> this also acts as the "migrated" flag (next time exists() == false -> no-op).
    fs::remove_all(secretsDir, ec);
}

} // namespace core
