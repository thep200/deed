#include "core/persistence/stores.hpp"
#include "infra/fs_util.hpp"
#include "codec/json_codec.hpp"

namespace core {

namespace {
std::string sessionPath(const std::string& root) {
    return fsutil::join(fsutil::join(root, ".session"), "session.json");
}
} // namespace

SessionStore::SessionStore(std::string root) : root_(std::move(root)) {
    current_ = load();
}

void SessionStore::setRoot(std::string root) {
    root_ = std::move(root);
    current_ = load();
}

Session SessionStore::load() const {
    // Fail-safe: thiếu/hỏng -> mặc định, không throw (README §6.3).
    std::string txt;
    if (!fsutil::readFile(sessionPath(root_), txt)) return Session{};
    try {
        return codec::sessionFromJson(codec::json::parse(txt));
    } catch (...) {
        return Session{}; // file hỏng -> khởi động sạch
    }
}

void SessionStore::persist() const {
    fsutil::writeFileAtomic(sessionPath(root_), codec::toJson(current_).dump(2));
}

std::string SessionStore::loadLastOpened() const { return current_.lastOpenedFile; }

void SessionStore::saveLastOpened(const std::string& relPath) {
    current_.lastOpenedFile = relPath;
    persist();
}

std::string SessionStore::getActiveEnv() const { return current_.activeEnv; }

void SessionStore::setActiveEnv(const std::string& name) {
    current_.activeEnv = name;
    persist();
}

} // namespace core
