#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/infra/persistence/stores.hpp"
#include "infra/persistence/env_crypto.hpp"
#include "infra/platform/fs_util.hpp"
#include "infra/serialization/json_codec.hpp"

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
        if (p.extension() != ".json") continue;
        std::string name = p.stem().string();
        if (name == kGlobalEnvName) continue; // reserved base — never listed
        out.push_back(std::move(name));
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
    if (appCfg_) {
        AppConfig cfg = appCfg_->load();
        if (!cfg.encryptionKey.empty())
            for (auto& k : e.keys)
                if (envcrypto::isEncrypted(k.value))
                    if (auto p = envcrypto::decrypt(k.value, cfg.encryptionKey)) k.value = *p;
        // wrong/missing key -> value stays "enc:v1:..." (visible, not lost)
    }
    return e;
}

bool EnvironmentStore::isEncryptedValue(const std::string& value) { return envcrypto::isEncrypted(value); }

Environment EnvironmentStore::encryptedForDisk(const Environment& e) const {
    if (!appCfg_) return e;
    AppConfig cfg = appCfg_->load();
    if (cfg.encryptionKey.empty()) return e;
    // Stored ciphertext reused when the plaintext is unchanged -> only actually-changed keys re-encrypt
    // (no nonce re-roll, byte-stable files).
    std::unordered_map<std::string, std::string> stored;
    {
        std::string txt;
        if (fsutil::readFile(envFile(root_, e.name), txt)) {
            try {
                Environment prev = codec::envFromJson(codec::parseGuarded(txt));
                for (auto& k : prev.keys)
                    if (envcrypto::isEncrypted(k.value)) stored.emplace(k.key, std::move(k.value));
            } catch (...) {}
        }
    }
    Environment out = e;
    for (auto& k : out.keys) {
        if (!k.secret || k.value.empty() || envcrypto::isEncrypted(k.value)) continue;
        if (auto it = stored.find(k.key); it != stored.end()) {
            if (auto p = envcrypto::decrypt(it->second, cfg.encryptionKey); p && *p == k.value) {
                k.value = it->second;   // unchanged -> keep stored bytes
                continue;
            }
        }
        k.value = envcrypto::encrypt(k.value, cfg.encryptionKey);
    }
    return out;
}

void EnvironmentStore::save(const Environment& e) {
    if (e.name.empty()) throw std::runtime_error("env must have a name");
    fsutil::writeFileAtomic(envFile(root_, e.name), codec::toJson(encryptedForDisk(e)).dump(2));
    epoch_.fetch_add(1, std::memory_order_relaxed);
}

void EnvironmentStore::remove(const std::string& name) {
    std::error_code ec;
    fs::remove(fs::path(envFile(root_, name)), ec);
    epoch_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace core
