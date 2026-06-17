#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "core/persistence/stores.hpp"
#include "infra/fs_util.hpp"
#include "codec/json_codec.hpp"

namespace fs = std::filesystem;

namespace core {

EnvironmentStore::EnvironmentStore(std::string root, std::shared_ptr<SecretStore> secrets)
    : root_(std::move(root)), secrets_(std::move(secrets)) {}

void EnvironmentStore::setRoot(std::string root) { root_ = std::move(root); }

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
    Environment e = codec::envFromJson(codec::json::parse(txt));
    if (e.name.empty()) e.name = name;
    // Bù value cho key secret từ SecretStore (để Engine resolve được).
    if (secrets_) {
        for (auto& k : e.keys) {
            if (k.secret) k.value = secrets_->get(name, k.key);
        }
    }
    return e;
}

void EnvironmentStore::save(const Environment& e) {
    if (e.name.empty()) throw std::runtime_error("env must have a name");
    if (e.name == "Global" && false) {} // Global cũng lưu file được; UI khoá rename/xoá
    // Secret -> SecretStore; file env chỉ giữ cờ + non-secret (codec đã bỏ value khi secret).
    if (secrets_) {
        for (const auto& k : e.keys) {
            if (k.secret) secrets_->set(e.name, k.key, k.value);
        }
    }
    fsutil::writeFileAtomic(envFile(root_, e.name), codec::toJson(e).dump(2));
}

void EnvironmentStore::remove(const std::string& name) {
    std::error_code ec;
    fs::remove(fs::path(envFile(root_, name)), ec);
}

} // namespace core
