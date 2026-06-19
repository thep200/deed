// core/stores.hpp — Các store của Core (README §6, §8.3, UI spec §2.3).
// Tất cả I/O file nằm trong Core; UI chỉ gọi load/save. Atomic write cho mọi ghi.
#pragma once

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

    // Quét MỘT cấp thư mục, CHỈ metadata (parse tên file, KHÔNG đọc nội dung).
    // dirRelPath rỗng = cấp gốc. Folder con để trạng thái fold (children rỗng). §3.
    std::vector<TreeNode> scanLevel(const std::string& dirRelPath) const;

    // Quét đệ quy toàn bộ (metadata-only, content-free). Dùng cho test/tiện ích;
    // UI dựng cây bằng scanLevel lazy thay vì hàm này.
    TreeNode scanTree() const;

    RequestModel loadRequest(const std::string& relPath) const;     // throws nếu lỗi
    // Ghi atomic; nếu tên file lệch type/method/name -> đổi tên cho khớp (§4).
    // Trả relPath SAU khi ghi (có thể đổi nếu rename). Atomic.
    std::string saveRequest(const std::string& relPath, const RequestModel&) const;

    // CRUD — trả relPath của item vừa tạo/đổi.
    std::string createRequest(const std::string& folderRel, RequestType, const std::string& name) const;
    // Tạo request mới TỪ model có sẵn (vd import cURL/grpcurl). Gán id mới + name, ghi atomic.
    std::string createRequestFromModel(const std::string& folderRel, RequestModel model,
                                       const std::string& name) const;
    std::string createFolder(const std::string& parentRel, const std::string& name) const;
    std::string rename(const std::string& relPath, const std::string& newName) const;
    std::string duplicate(const std::string& relPath) const;
    void remove(const std::string& relPath) const;
    // Di chuyển request/folder vào folder đích (drag-drop). Trả relPath mới.
    std::string move(const std::string& relPath, const std::string& destFolderRel) const;

    // Tìm relPath của request theo id ổn định (id ưu tiên đọc từ tên file). Rỗng nếu không thấy.
    std::string findRelPathById(const std::string& id) const;

    // Migrate 1 lần: thêm <id> vào ĐẦU tên file cho các file CŨ chưa có id (git mv).
    // File đã có id trong tên -> bỏ qua KHÔNG đọc nội dung. Trả số file đã đổi tên. §2A.
    int migrateAddIdToFilenames() const;

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
// Giá trị biến lưu plaintext trong chính file env (cơ chế secret đã gỡ — SPEC §T5).
class EnvironmentStore {
public:
    explicit EnvironmentStore(std::string root);
    void setRoot(std::string root);

    std::vector<std::string> list() const;          // tên env (không gồm "Global" ảo)
    Environment load(const std::string& name) const;
    void save(const Environment&);                   // atomic
    void remove(const std::string& name);

    // Đổi tên env (rename file atomic). Trả false nếu rỗng/trùng tên env khác.
    bool renameEnv(const std::string& oldName, const std::string& newName);
    // Đổi key alias trên TẤT CẢ env cùng lúc (alias là khoá hàng dùng chung).
    // Trả false nếu rỗng/trùng alias khác trên bất kỳ env nào.
    bool renameAlias(const std::string& oldAlias, const std::string& newAlias);

    // Migration một lần (SPEC §T5): gộp value trong .secrets/secrets.json (định dạng
    // cũ {env:{key:value}}) ngược vào file env như biến thường, rồi xoá .secrets/.
    // No-op nếu .secrets/ không tồn tại (đã migrate hoặc chưa từng có secret).
    void migrateLegacySecrets();

private:
    std::string root_;
};

// AppConfigStore — app-global ở OS app-support, NGOÀI collection (README §12.1).
class AppConfigStore {
public:
    AppConfigStore();
    explicit AppConfigStore(std::string path); // override (test)
    // Giá trị mặc định (từ .env, UI nạp) dùng khi config.json thiếu/khuyết key.
    void setDefaults(const AppConfig& d) { defaults_ = d; }
    const AppConfig& defaults() const { return defaults_; }
    AppConfig load() const;                     // thiếu key/file -> rơi về defaults_
    void save(const AppConfig&) const;          // atomic
    const std::string& path() const { return path_; }
private:
    std::string path_;
    AppConfig defaults_;
};

} // namespace core
