#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/domain/request/request_model.hpp"
#include "core/domain/environment/env_config.hpp"
#include "core/domain/request/request_type.hpp"

namespace core {

// Fallbacks when the .env value (DEFAULT_TIMEOUT_MS / VERIFY_TLS) is absent or 0.
inline constexpr long long kNewRequestTimeoutMsDefault = 30LL * 60 * 1000; // 30 min
inline constexpr bool kNewRequestVerifyTlsDefault = true;

// On-disk: folder tree; filename = slug; display name lives in the "name" field.
class CollectionStore {
public:
    explicit CollectionStore(std::string root);

    const std::string& root() const { return root_; }
    void setRoot(std::string root);

    // ONE directory level, metadata ONLY (parse filename, do NOT read content); "" = root; subfolders kept folded.
    std::vector<TreeNode> scanLevel(const std::string& dirRelPath) const;

    // Recursive, metadata-only; the UI uses lazy scanLevel instead.
    TreeNode scanTree() const;

    core::domain::RequestModel loadRequest(const std::string& relPath) const;     // throws on error
    // Atomic; renames the file to match type/method/name and returns relPath AFTER write. Preserves "_uiBodyDrafts".
    std::string saveRequest(const std::string& relPath, const core::domain::RequestModel&) const;

    // UI-only per-mode body drafts persisted under "_uiBodyDrafts"; the domain model + senders IGNORE them.
    // Empty map when the key is missing.
    std::map<std::string, std::string> loadBodyDrafts(const std::string& relPath) const;
    // Also writes "_uiBodyDrafts" (overwriting); empty drafts -> the key is omitted.
    std::string saveRequest(const std::string& relPath, const core::domain::RequestModel&,
                            const std::map<std::string, std::string>& bodyDrafts) const;

    // timeoutMs <= 0 keeps the built-in default; affects createRequest only — existing requests load their own config.
    void setRequestDefaults(long long timeoutMs, bool verifyTls);

    // CRUD — return relPath of the created/changed item.
    std::string createRequest(const std::string& folderRel, RequestType, const std::string& name) const;
    // Assigns a new id + name (import path); atomic write.
    std::string createRequestFromModel(const std::string& folderRel, core::domain::RequestModel model,
                                       const std::string& name) const;
    std::string createFolder(const std::string& parentRel, const std::string& name) const;
    std::string rename(const std::string& relPath, const std::string& newName) const;
    std::string duplicate(const std::string& relPath) const;
    void remove(const std::string& relPath) const;
    std::string move(const std::string& relPath, const std::string& destFolderRel) const;

    // Places the entry at `index` of the level's sorted children; renames exactly ONE entry (fractional index).
    std::string reorder(const std::string& relPath, const std::string& destFolderRel, int index) const;

    // Empty if not found.
    std::string findRelPathById(const std::string& id) const;

    // Ensures .gitignore covers .session/ and .secrets/.
    void ensureGitignore() const;

private:
    std::string root_;
    long long defaultTimeoutMs_ = kNewRequestTimeoutMsDefault; // new-request default timeout (.env DEFAULT_TIMEOUT_MS)
    bool defaultVerifyTls_ = kNewRequestVerifyTlsDefault;      // new-request default TLS verify (.env VERIFY_TLS)

    // id->relPath index built LAZILY from filenames (zero-read); invalidated on every mutation.
    // Lookup verifies the file still exists and rebuilds if stale — safe against out-of-app changes.
    mutable std::mutex idMu_;
    mutable std::unordered_map<std::string, std::string> idIndex_;
    mutable bool idIndexBuilt_ = false;
    void buildIdIndexLocked() const;     // rebuild idIndex_ (call WHILE holding idMu_)
    void invalidateIdIndex() const;      // mark for rebuild (after mutation)
};

// App-state (.session/session.json), NOT git-versioned.
// Writes are DEBOUNCED onto one worker thread; flush()/destructor writes any pending.
class SessionStore {
public:
    explicit SessionStore(std::string root);
    ~SessionStore();                                 // flush pending + stop worker
    void setRoot(std::string root);                  // flush old root before switching

    // Fail-safe: missing/corrupt file -> return default Session, no throw.
    Session load() const;

    std::string loadLastOpened() const;
    void saveLastOpened(const std::string& relPath); // debounce
    std::string getActiveEnv() const;
    void setActiveEnv(const std::string& name);      // debounce

    void flush();                                    // write NOW if pending (quit/switch collection)

private:
    Session current_;
    std::string root_;

    mutable std::mutex mu_;                           // guards current_/root_/flags; worker + main access it
    std::condition_variable cv_;
    std::thread worker_;
    bool dirty_ = false;                              // has unwritten changes
    bool stop_ = false;                               // worker stop signal (destructor)
    void markDirtyLocked();                           // set dirty_ + wake worker (hold lock when calling)
    void workerLoop();                                // wait for debounce then write snapshot
    static void writeSnapshot(const std::string& root, const Session& s);  // atomic write (outside lock)
};

class AppConfigStore;

// One file per env; "Enc"-flagged values are encrypted at rest when a key is configured — in-memory always plaintext.
class EnvironmentStore {
public:
    explicit EnvironmentStore(std::string root);
    void setRoot(std::string root);
    // Source of encryption_key; null = plaintext.
    void attachAppConfig(const AppConfigStore* cfg) { appCfg_ = cfg; }
    // True if a loaded value is still ciphertext -> the configured key can't read it (UI warns).
    static bool isEncryptedValue(const std::string& value);

    std::vector<std::string> list() const;          // env names (reserved "Global" excluded)
    Environment load(const std::string& name) const;
    void save(const Environment&);                   // atomic
    void remove(const std::string& name);
    // (env/alias rename = UI view-model save-new + delete-old)

    // Bumps on every env change. mergedVars caches on (activeEnv, epoch) -> no disk read per send.
    std::uint64_t epoch() const { return epoch_.load(std::memory_order_relaxed); }

private:
    Environment encryptedForDisk(const Environment&) const; // Enc-flagged values -> ciphertext (unless excluded)
    std::string root_;
    std::atomic<std::uint64_t> epoch_{0};
    const AppConfigStore* appCfg_ = nullptr;
};

// App-global, in OS app-support — OUTSIDE the collection.
class AppConfigStore {
public:
    AppConfigStore();
    explicit AppConfigStore(std::string path); // override (test)
    // Default values (from .env, loaded by UI) used when config.json misses/omits a key.
    void setDefaults(const AppConfig& d) { defaults_ = d; }
    const AppConfig& defaults() const { return defaults_; }
    AppConfig load() const;                     // missing key/file -> fall back to defaults_
    void save(const AppConfig&) const;          // atomic
    const std::string& path() const { return path_; }
private:
    std::string path_;
    AppConfig defaults_;
};

} // namespace core
