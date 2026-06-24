#include <nlohmann/json.hpp>

#include "core/persistence/stores.hpp"
#include "infra/fs_util.hpp"
#include "codec/json_codec.hpp"

namespace core {

AppConfigStore::AppConfigStore()
    : path_(fsutil::join(fsutil::appSupportDir("deed"), "config.json")) {}

AppConfigStore::AppConfigStore(std::string path) : path_(std::move(path)) {}

AppConfig AppConfigStore::load() const {
    std::string txt;
    if (!fsutil::readFile(path_, txt)) return defaults_;   // no file yet -> defaults (.env)
    try {
        return codec::appConfigFromJson(codec::parseGuarded(txt), defaults_);
    } catch (...) {
        return defaults_;
    }
}

void AppConfigStore::save(const AppConfig& c) const {
    fsutil::writeFileAtomic(path_, codec::toJson(c).dump(2));
}

} // namespace core
