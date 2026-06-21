// core/stores.hpp — Core's stores (README §6, §8.3, UI spec §2.3).
// All file I/O lives in Core; UI only calls load/save. Atomic write for every write.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"

namespace core {

// CollectionStore — load/save requests, scan the tree (README §6.2).
// Real on-disk path = folder tree; filename = slug; display name in the name field.
class CollectionStore {
public:
    explicit CollectionStore(std::string root);

    const std::string& root() const { return root_; }
    void setRoot(std::string root);

    // Scan ONE directory level, metadata ONLY (parse filename, do NOT read content).
    // empty dirRelPath = root level. Subfolders kept folded (empty children). §3.
    std::vector<TreeNode> scanLevel(const std::string& dirRelPath) const;

    // Scan the whole tree recursively (metadata-only, content-free). For test/utility;
    // UI builds the tree via lazy scanLevel instead of this.
    TreeNode scanTree() const;

    RequestModel loadRequest(const std::string& relPath) const;     // throws on error
    // Atomic write; if filename mismatches type/method/name -> rename to match (§4).
    // Returns relPath AFTER write (may change on rename). Atomic.
    std::string saveRequest(const std::string& relPath, const RequestModel&) const;

    // CRUD — return relPath of the created/changed item.
    std::string createRequest(const std::string& folderRel, RequestType, const std::string& name) const;
    // Create a new request FROM an existing model (e.g. cURL/grpcurl import). Assign new id + name, atomic write.
    std::string createRequestFromModel(const std::string& folderRel, RequestModel model,
                                       const std::string& name) const;
    std::string createFolder(const std::string& parentRel, const std::string& name) const;
    std::string rename(const std::string& relPath, const std::string& newName) const;
    std::string duplicate(const std::string& relPath) const;
    void remove(const std::string& relPath) const;
    // Move a request/folder into the destination folder (drag-drop). Returns new relPath.
    std::string move(const std::string& relPath, const std::string& destFolderRel) const;

    // Find a request's relPath by stable id (id preferably read from filename). Empty if not found.
    std::string findRelPathById(const std::string& id) const;

    // One-time migrate: prepend <id> to the filename for OLD files lacking an id (git mv).
    // Files that already have an id -> skipped WITHOUT reading content. Returns count renamed. §2A.
    int migrateAddIdToFilenames() const;

    // Ensure .gitignore has entries for .session/ and .secrets/ (app-managed — README §6.3).
    void ensureGitignore() const;

private:
    std::string root_;

    // id->relPath index built LAZILY from filenames (zero-read after migrate) -> findRelPathById O(1)
    // instead of scanning the WHOLE tree each call (resyncCurrentRelById runs before every save/switch).
    // Invalidated on mutation (create/rename/move/duplicate/remove/save/migrate/setRoot); lookup
    // also verifies the file still exists -> rebuilds itself if stale (safe against out-of-app changes).
    mutable std::mutex idMu_;
    mutable std::unordered_map<std::string, std::string> idIndex_;
    mutable bool idIndexBuilt_ = false;
    void buildIdIndexLocked() const;     // rebuild idIndex_ (call WHILE holding idMu_)
    void invalidateIdIndex() const;      // mark for rebuild (after mutation)
};

// SessionStore — app-state (.session/session.json), NOT git-versioned.
// Writes are DEBOUNCED: many consecutive changes (e.g. clicking through requests) coalesce into one disk write
// after ~kDebounceMs, handled by one worker thread. flush()/destructor writes out any pending.
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

    // --- Disk-write debounce ---
    mutable std::mutex mu_;                           // guards current_/root_/flags; worker + main access it
    std::condition_variable cv_;
    std::thread worker_;
    bool dirty_ = false;                              // has unwritten changes
    bool stop_ = false;                               // worker stop signal (destructor)
    void markDirtyLocked();                           // set dirty_ + wake worker (hold lock when calling)
    void workerLoop();                                // wait for debounce then write snapshot
    static void writeSnapshot(const std::string& root, const Session& s);  // atomic write (outside lock)
};

// EnvironmentStore — one file per env in environments/ (README §8.3).
// Variable values stored plaintext in the env file itself (secret mechanism removed — SPEC §T5).
class EnvironmentStore {
public:
    explicit EnvironmentStore(std::string root);
    void setRoot(std::string root);

    std::vector<std::string> list() const;          // env names (excluding the virtual "Global")
    Environment load(const std::string& name) const;
    void save(const Environment&);                   // atomic
    void remove(const std::string& name);

    // Rename an env (atomic file rename). Returns false if empty/clashes with another env name.
    bool renameEnv(const std::string& oldName, const std::string& newName);
    // Rename a key alias across ALL envs at once (alias is the shared row key).
    // Returns false if empty/clashes with another alias on any env.
    bool renameAlias(const std::string& oldAlias, const std::string& newAlias);

    // One-time migration (SPEC §T5): fold values from .secrets/secrets.json (old
    // {env:{key:value}} format) back into the env file as normal vars, then delete .secrets/.
    // No-op if .secrets/ doesn't exist (already migrated or never had secrets).
    void migrateLegacySecrets();

    // Epoch bumps EACH time env content changes (save/remove/rename/renameAlias/migrate/setRoot).
    // Engine uses it to cache merged-vars: cache hit when (activeEnv, epoch) unchanged -> avoid re-reading
    // disk on every send/resolve; any env edit bumps epoch -> cache self-invalidates (no stale vars).
    std::uint64_t epoch() const { return epoch_.load(std::memory_order_relaxed); }

private:
    std::string root_;
    std::atomic<std::uint64_t> epoch_{0};
};

// AppConfigStore — app-global in OS app-support, OUTSIDE the collection (README §12.1).
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
