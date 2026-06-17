#include <nlohmann/json.hpp>

#include "core/persistence/stores.hpp"
#include "infra/fs_util.hpp"

namespace core {

using nlohmann::json;

FileSecretStore::FileSecretStore(std::string root) : root_(std::move(root)) {}
void FileSecretStore::setRoot(std::string root) { root_ = std::move(root); }

std::string FileSecretStore::filePath() const {
    return fsutil::join(fsutil::join(root_, ".secrets"), "secrets.json");
}

namespace {
// Layout: { "<env>": { "<key>": "<value>" } }
json loadSecrets(const std::string& path) {
    std::string txt;
    if (!fsutil::readFile(path, txt)) return json::object();
    try {
        auto j = json::parse(txt);
        return j.is_object() ? j : json::object();
    } catch (...) { return json::object(); }
}
} // namespace

std::string FileSecretStore::get(const std::string& env, const std::string& key) {
    auto j = loadSecrets(filePath());
    if (auto e = j.find(env); e != j.end() && e->is_object()) {
        if (auto k = e->find(key); k != e->end() && k->is_string())
            return k->get<std::string>();
    }
    return "";
}

void FileSecretStore::set(const std::string& env, const std::string& key, const std::string& value) {
    auto j = loadSecrets(filePath());
    j[env][key] = value;
    fsutil::writeFileAtomic(filePath(), j.dump(2));
}

void FileSecretStore::remove(const std::string& env, const std::string& key) {
    auto j = loadSecrets(filePath());
    if (auto e = j.find(env); e != j.end() && e->is_object()) {
        e->erase(key);
        if (e->empty()) j.erase(env);
        fsutil::writeFileAtomic(filePath(), j.dump(2));
    }
}

} // namespace core
