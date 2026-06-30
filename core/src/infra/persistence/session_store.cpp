#include "core/infra/persistence/stores.hpp"

#include <chrono>

#include "infra/platform/fs_util.hpp"
#include "infra/serialization/json_codec.hpp"

namespace core {

namespace {
std::string sessionPath(const std::string& root) {
    return fsutil::join(fsutil::join(root, ".session"), "session.json");
}
// Write-coalescing window: changes within this span collapse into one disk write.
constexpr auto kDebounce = std::chrono::milliseconds(400);
} // namespace

SessionStore::SessionStore(std::string root) : root_(std::move(root)) {
    current_ = load();
    worker_ = std::thread(&SessionStore::workerLoop, this);
}

SessionStore::~SessionStore() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    flush();   // write any pending state (worker does not write on stop -> avoid double-write)
}

Session SessionStore::load() const {
    // Fail-safe: missing/corrupt -> defaults, no throw (README §6.3).
    std::string txt;
    if (!fsutil::readFile(sessionPath(root_), txt)) return Session{};
    try {
        return codec::sessionFromJson(codec::parseGuarded(txt));
    } catch (...) {
        return Session{}; // corrupt file -> start clean
    }
}

void SessionStore::writeSnapshot(const std::string& root, const Session& s) {
    try { fsutil::writeFileAtomic(sessionPath(root), codec::toJson(s).dump(2)); }
    catch (...) { /* session is auxiliary state; write error -> ignore, retry next time */ }
}

void SessionStore::markDirtyLocked() {
    dirty_ = true;
    cv_.notify_all();   // wake the worker to start the debounce window
}

void SessionStore::workerLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait(lk, [&] { return stop_ || dirty_; });
        if (stop_) return;                                  // destructor flushes the pending part
        // Change pending: wait another kDebounce to coalesce following changes (exit early if stop).
        cv_.wait_for(lk, kDebounce, [&] { return stop_; });
        if (stop_) return;
        if (dirty_) {                                       // write the latest snapshot OUTSIDE the lock
            Session snap = current_;
            std::string root = root_;
            dirty_ = false;
            lk.unlock();
            writeSnapshot(root, snap);
            lk.lock();
        }
    }
}

void SessionStore::flush() {
    Session snap;
    std::string root;
    bool need = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (dirty_) { snap = current_; root = root_; dirty_ = false; need = true; }
    }
    if (need) writeSnapshot(root, snap);
}

void SessionStore::setRoot(std::string root) {
    flush();   // write the old collection's state before switching
    std::lock_guard<std::mutex> lk(mu_);
    root_ = std::move(root);
    current_ = load();
}

std::string SessionStore::loadLastOpened() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_.lastOpenedFile;
}

void SessionStore::saveLastOpened(const std::string& relPath) {
    std::lock_guard<std::mutex> lk(mu_);
    if (current_.lastOpenedFile == relPath) return;   // unchanged -> no write
    current_.lastOpenedFile = relPath;
    markDirtyLocked();
}

std::string SessionStore::getActiveEnv() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_.activeEnv;
}

void SessionStore::setActiveEnv(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (current_.activeEnv == name) return;            // unchanged -> no write
    current_.activeEnv = name;
    markDirtyLocked();
}

} // namespace core
