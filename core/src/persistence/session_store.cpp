#include "core/persistence/stores.hpp"

#include <chrono>

#include "infra/fs_util.hpp"
#include "codec/json_codec.hpp"

namespace core {

namespace {
std::string sessionPath(const std::string& root) {
    return fsutil::join(fsutil::join(root, ".session"), "session.json");
}
// Cửa sổ gộp ghi: thay đổi trong khoảng này dồn vào 1 lần ghi đĩa.
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
    flush();   // ghi nốt nếu còn treo (worker không ghi lúc stop -> tránh ghi 2 lần)
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

void SessionStore::writeSnapshot(const std::string& root, const Session& s) {
    try { fsutil::writeFileAtomic(sessionPath(root), codec::toJson(s).dump(2)); }
    catch (...) { /* session là state phụ trợ; lỗi ghi -> bỏ qua, sẽ thử lại lần sau */ }
}

void SessionStore::markDirtyLocked() {
    dirty_ = true;
    cv_.notify_all();   // đánh thức worker để bắt đầu cửa sổ debounce
}

void SessionStore::workerLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait(lk, [&] { return stop_ || dirty_; });
        if (stop_) return;                                  // destructor flush phần treo
        // Có thay đổi: chờ thêm kDebounce để gộp các thay đổi kế tiếp (thoát sớm nếu stop).
        cv_.wait_for(lk, kDebounce, [&] { return stop_; });
        if (stop_) return;
        if (dirty_) {                                       // ghi snapshot mới nhất NGOÀI lock
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
    flush();   // ghi state của collection cũ trước khi chuyển
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
    if (current_.lastOpenedFile == relPath) return;   // không đổi -> khỏi ghi
    current_.lastOpenedFile = relPath;
    markDirtyLocked();
}

std::string SessionStore::getActiveEnv() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_.activeEnv;
}

void SessionStore::setActiveEnv(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (current_.activeEnv == name) return;            // không đổi -> khỏi ghi
    current_.activeEnv = name;
    markDirtyLocked();
}

} // namespace core
