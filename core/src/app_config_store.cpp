#include <nlohmann/json.hpp>

#include "core/stores.hpp"
#include "fs_util.hpp"
#include "json_codec.hpp"

namespace core {

AppConfigStore::AppConfigStore()
    : path_(fsutil::join(fsutil::appSupportDir("deed"), "config.json")) {}

AppConfigStore::AppConfigStore(std::string path) : path_(std::move(path)) {}

AppConfig AppConfigStore::load() const {
    std::string txt;
    if (!fsutil::readFile(path_, txt)) return AppConfig{};
    try {
        return codec::appConfigFromJson(nlohmann::json::parse(txt));
    } catch (...) {
        return AppConfig{};
    }
}

void AppConfigStore::save(const AppConfig& c) const {
    fsutil::writeFileAtomic(path_, codec::toJson(c).dump(2));
}

} // namespace core
