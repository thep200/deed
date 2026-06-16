// core/stores.hpp — Các store của Core (README §6, §8.3, UI spec §2.3).
// Tất cả I/O file nằm trong Core; UI chỉ gọi load/save. Atomic write cho mọi ghi.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace core {

// CollectionStore — load/save request, quét cây (README §6.2).
// Đường thật trên đĩa = cây folder; tên file = slug; tên hiển thị ở field name.
class CollectionStore {
public:
    explicit CollectionStore(std::string root);

    const std::string& root() const { return root_; }
    void setRoot(std::string root);

    // Lazy: chỉ đọc metadata (name/path/type) để dựng sidebar.
    TreeNode scanTree() const;

    RequestModel loadRequest(const std::string& relPath) const;     // throws nếu lỗi
    void saveRequest(const std::string& relPath, const RequestModel&) const; // atomic

    // CRUD — trả relPath của item vừa tạo/đổi.
    std::string createRequest(const std::string& folderRel, RequestType, const std::string& name) const;
    std::string createFolder(const std::string& parentRel, const std::string& name) const;
    std::string rename(const std::string& relPath, const std::string& newName) const;
    std::string duplicate(const std::string& relPath) const;
    void remove(const std::string& relPath) const;
    // Di chuyển request/folder vào folder đích (drag-drop). Trả relPath mới.
    std::string move(const std::string& relPath, const std::string& destFolderRel) const;

    // Tìm relPath của request theo id ổn định (quét cây). Rỗng nếu không thấy.
    std::string findRelPathById(const std::string& id) const;

    // Đảm bảo .gitignore có entry cho .session/ và .secrets/ (app tự quản — README §6.3).
    void ensureGitignore() const;

private:
    std::string root_;
};

// SessionStore — app-state (.session/session.json), KHÔNG version git.
class SessionStore {
public:
    explicit SessionStore(std::string root);
    void setRoot(std::string root);

    // Fail-safe: file thiếu/hỏng -> trả Session mặc định, không throw.
    Session load() const;

    std::string loadLastOpened() const;
    void saveLastOpened(const std::string& relPath); // atomic
    std::string getActiveEnv() const;
    void setActiveEnv(const std::string& name);      // atomic

private:
    Session current_;
    std::string root_;
    void persist() const;
};

// EnvironmentStore — mỗi env một file trong environments/ (README §8.3).
// Secret KHÔNG ghi vào file env; đẩy qua SecretStore.
class SecretStore; // fwd

class EnvironmentStore {
public:
    EnvironmentStore(std::string root, std::shared_ptr<SecretStore> secrets);
    void setRoot(std::string root);

    std::vector<std::string> list() const;          // tên env (không gồm "Global" ảo)
    Environment load(const std::string& name) const;
    void save(const Environment&);                   // atomic; secret -> SecretStore
    void remove(const std::string& name);

private:
    std::string root_;
    std::shared_ptr<SecretStore> secrets_;
};

// SecretStore — tách dữ liệu nhạy cảm khỏi session (README §8.3).
class SecretStore {
public:
    virtual ~SecretStore() = default;
    virtual std::string get(const std::string& env, const std::string& key) = 0;
    virtual void set(const std::string& env, const std::string& key, const std::string& value) = 0;
    virtual void remove(const std::string& env, const std::string& key) = 0;
};

// FileSecretStore — POC: .secrets/secrets.json (git-ignored).
class FileSecretStore : public SecretStore {
public:
    explicit FileSecretStore(std::string root);
    void setRoot(std::string root);
    std::string get(const std::string& env, const std::string& key) override;
    void set(const std::string& env, const std::string& key, const std::string& value) override;
    void remove(const std::string& env, const std::string& key) override;
private:
    std::string root_;
    std::string filePath() const;
};

// AppConfigStore — app-global ở OS app-support, NGOÀI collection (README §12.1).
class AppConfigStore {
public:
    AppConfigStore();
    explicit AppConfigStore(std::string path); // override (test)
    AppConfig load() const;
    void save(const AppConfig&) const;          // atomic
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

} // namespace core
