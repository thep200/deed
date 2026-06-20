#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>

#include "core/persistence/stores.hpp"
#include "core/persistence/request_naming.hpp"
#include "infra/fs_util.hpp"
#include "codec/json_codec.hpp"

namespace fs = std::filesystem;

namespace core {

namespace {

// Thư mục/đặc biệt KHÔNG hiện trong cây request.
bool isReservedDir(const std::string& name) {
    return name == ".session" || name == ".secrets" || name == ".git" ||
           name == "environments";
}
// File config cấp collection/folder -> không phải request.
bool isConfigFile(const std::string& name) {
    return name == "collection.json" || name == "folder.json";
}
bool isHidden(const std::string& name) { return !name.empty() && name[0] == '.'; }

// Sinh id ngẫu nhiên nhẹ: base36 LOWERCASE, 12 ký tự, KHÔNG '_' (để nhúng vào TÊN FILE — §2A).
// RNG seed MỘT LẦN rồi tiến trạng thái mỗi lần gọi -> không trùng dù gọi liên tiếp.
std::string genId() {
    static std::mutex mu;
    static std::mt19937_64 rng([] {
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        s ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return s;
    }());
    static const char* alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"; // base36, [a-z0-9]
    std::lock_guard<std::mutex> lk(mu);
    std::string s;
    for (int i = 0; i < 12; ++i) s += alphabet[rng() % 36];
    return s;
}

// id dùng được trong tên file? (cũ "req_..." chứa '_' -> không hợp lệ -> phải regenerate khi migrate).
std::string ensureFileId(const std::string& id) { return isValidFileId(id) ? id : genId(); }

std::string uniquePath(const fs::path& dir, const std::string& slug, const std::string& ext) {
    fs::path cand = dir / (slug + ext);
    if (!fs::exists(cand)) return cand.string();
    for (int i = 2; i < 10000; ++i) {
        fs::path c = dir / (slug + "-" + std::to_string(i) + ext);
        if (!fs::exists(c)) return c.string();
    }
    throw std::runtime_error("could not find a unique filename for: " + slug);
}

// Tìm tên file duy nhất theo grammar mã hoá (<id>_type_..., id đầu). id duy nhất nên hầu như
// không trùng; vẫn giữ vòng thêm hậu tố slug cho chắc. Trả TÊN FILE (không kèm thư mục).
std::string uniqueEncodedName(const fs::path& dir, const std::string& id, RequestType type,
                              const std::string& method, const std::string& displayName) {
    std::string first = encodeRequestFilename(id, type, method, displayName);
    if (!fs::exists(dir / first)) return first;
    for (int i = 2; i < 10000; ++i) {
        std::string cand = encodeRequestFilename(id, type, method, displayName + " " + std::to_string(i));
        if (!fs::exists(dir / cand)) return cand;
    }
    throw std::runtime_error("could not find a unique filename for: " + displayName);
}

// Dựng metadata leaf cho 1 file request — KHÔNG đọc nội dung khi tên file hợp lệ (§2).
// Chỉ fallback đọc 1 lần nếu tên file sai grammar (§5).
TreeNode buildRequestLeaf(const fs::path& fullPath, const std::string& relPath) {
    TreeNode leaf;
    leaf.isFolder = false;
    leaf.relPath = relPath;
    std::string fname = fullPath.filename().string();

    ParsedRequestName p = parseRequestFilename(fname);
    if (p.ok) {
        leaf.requestType = p.type;
        leaf.name = normalizeDisplayName(p.slug);
        if (p.type == RequestType::Http) {
            std::string m = p.method;
            for (auto& c : m) c = static_cast<char>(std::toupper((unsigned char)c));
            leaf.methodOrType = m;            // badge HTTP method (GET/POST...)
        } else {
            leaf.methodOrType.clear();        // gRPC: UI không hiển thị method type
        }
        leaf.id = p.id;                       // id ĐỌC TỪ TÊN FILE (zero-read) — dùng cho reveal/cache.
        if (leaf.id.empty()) {                // file CŨ chưa có id trong tên -> đọc 1 lần lấy id nội dung
            std::string txt;
            if (fsutil::readFile(fullPath.string(), txt)) {
                try { leaf.id = codec::json::parse(txt).value("id", std::string()); } catch (...) {}
            }
        }
        return leaf;
    }

    // Fallback: tên file không đúng grammar -> đọc 1 lần để lấy type/method/name thật.
    leaf.name = fullPath.stem().string();     // tối thiểu: tên file (bỏ .json)
    std::string txt;
    if (fsutil::readFile(fullPath.string(), txt)) {
        try {
            auto j = codec::json::parse(txt);
            if (j.contains("name") && j["name"].is_string())
                leaf.name = j["name"].get<std::string>();
            leaf.id = j.value("id", std::string());
            std::string t = j.value("type", "http");
            parseRequestType(t, leaf.requestType);
            if (leaf.requestType == RequestType::Http)
                leaf.methodOrType = j.value("http", codec::json::object()).value("method", "GET");
            else
                leaf.methodOrType.clear();
        } catch (...) { /* file lỗi -> vẫn hiện tên file */ }
    }
    return leaf;
}

} // namespace

CollectionStore::CollectionStore(std::string root) : root_(std::move(root)) {}
void CollectionStore::setRoot(std::string root) { root_ = std::move(root); invalidateIdIndex(); }

// Quét 1 cấp — metadata-only. Folder con để fold (children rỗng). KHÔNG đọc nội dung
// để render (chỉ fallback khi tên file sai grammar, trong buildRequestLeaf). §3.
std::vector<TreeNode> CollectionStore::scanLevel(const std::string& dirRelPath) const {
    std::vector<TreeNode> out;
    fs::path dir = fs::path(fsutil::join(root_, dirRelPath));
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        // Không đi theo symlink (tránh đệ quy vòng — §10).
        if (e.is_symlink()) continue;
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.is_directory() != b.is_directory()) return a.is_directory(); // folder lên trước
        return a.path().filename().string() < b.path().filename().string();
    });
    for (const auto& e : entries) {
        std::string fname = e.path().filename().string();
        std::string childRel = dirRelPath.empty() ? fname : dirRelPath + "/" + fname;
        if (e.is_directory()) {
            if (isReservedDir(fname) || isHidden(fname)) continue;
            TreeNode folder;
            folder.isFolder = true;
            folder.relPath = childRel;
            folder.name = fname;                  // folder: tên thư mục (KHÔNG de-slug)
            out.push_back(std::move(folder));     // children rỗng -> lazy expand sau
        } else if (e.is_regular_file()) {
            if (e.path().extension() != ".json") continue;
            if (isConfigFile(fname) || isHidden(fname)) continue;
            out.push_back(buildRequestLeaf(e.path(), childRel));
        }
    }
    return out;
}

TreeNode CollectionStore::scanTree() const {
    std::function<TreeNode(const std::string&)> walk =
        [&](const std::string& rel) -> TreeNode {
        TreeNode node;
        node.isFolder = true;
        node.relPath = rel;
        node.name = rel.empty() ? fs::path(root_).filename().string()
                                : fs::path(rel).filename().string();
        for (auto& child : scanLevel(rel)) {
            if (child.isFolder) node.children.push_back(walk(child.relPath));
            else node.children.push_back(std::move(child));
        }
        return node;
    };
    return walk("");
}

RequestModel CollectionStore::loadRequest(const std::string& relPath) const {
    std::string txt;
    if (!fsutil::readFile(fsutil::join(root_, relPath), txt))
        throw std::runtime_error("cannot read request: " + relPath);
    RequestModel m = codec::requestFromJson(codec::json::parse(txt));
    // id phải hợp lệ để nhúng vào TÊN FILE ([a-z0-9], không '_'). Nội dung rỗng/legacy "req_..."
    // -> KHÔNG ghi đĩa ở đây (loadRequest là READ thuần — tránh write-storm khi mở/duyệt request).
    // Ưu tiên id TỪ TÊN FILE (đã ổn định sau migrateAddIdToFilenames); chỉ sinh tạm nếu tên file
    // cũng chưa có id. Việc rewrite nội dung do saveRequest/migrateAddIdToFilenames sở hữu.
    if (!isValidFileId(m.id)) {
        ParsedRequestName p = parseRequestFilename(fs::path(relPath).filename().string());
        m.id = (p.ok && isValidFileId(p.id)) ? p.id : genId();
    }
    return m;
}

// Dựng idIndex_ một lượt: id ưu tiên TỪ TÊN FILE (zero-read), chỉ đọc nội dung cho file legacy.
void CollectionStore::buildIdIndexLocked() const {
    idIndex_.clear();
    std::function<void(const std::string&)> walk = [&](const std::string& rel) {
        fs::path dir = fs::path(fsutil::join(root_, rel));
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_symlink()) continue;
            std::string fname = e.path().filename().string();
            std::string childRel = rel.empty() ? fname : rel + "/" + fname;
            if (e.is_directory()) {
                if (isReservedDir(fname) || isHidden(fname)) continue;
                walk(childRel);
            } else if (e.is_regular_file()) {
                if (e.path().extension() != ".json" || isConfigFile(fname) || isHidden(fname))
                    continue;
                ParsedRequestName p = parseRequestFilename(fname);
                std::string id = p.id;
                if (id.empty()) {                    // file cũ chưa có id trong tên -> đọc nội dung
                    std::string txt;
                    if (fsutil::readFile(e.path().string(), txt)) {
                        try { id = codec::json::parse(txt).value("id", std::string()); } catch (...) {}
                    }
                }
                if (!id.empty()) idIndex_.emplace(id, childRel);  // id đầu tiên thắng (ổn định)
            }
        }
    };
    walk("");
    idIndexBuilt_ = true;
}

void CollectionStore::invalidateIdIndex() const {
    std::lock_guard<std::mutex> lk(idMu_);
    idIndexBuilt_ = false;
}

std::string CollectionStore::findRelPathById(const std::string& id) const {
    if (id.empty()) return "";
    std::lock_guard<std::mutex> lk(idMu_);
    if (!idIndexBuilt_) buildIdIndexLocked();
    auto it = idIndex_.find(id);
    if (it == idIndex_.end()) return "";
    // Xác thực: file còn tồn tại đúng đường dẫn đã cache? (chống lệch nếu đổi NGOÀI app)
    // -> nếu mất, dựng lại 1 lần rồi tra lại; tránh trả path "ma".
    if (fs::exists(fs::path(fsutil::join(root_, it->second)))) return it->second;
    buildIdIndexLocked();
    auto it2 = idIndex_.find(id);
    return it2 != idIndex_.end() ? it2->second : "";
}

int CollectionStore::migrateAddIdToFilenames() const {
    int migrated = 0;
    std::function<void(const std::string&)> walk = [&](const std::string& rel) {
        fs::path dir = fs::path(fsutil::join(root_, rel));
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_symlink()) continue;
            std::string fname = e.path().filename().string();
            std::string childRel = rel.empty() ? fname : rel + "/" + fname;
            if (e.is_directory()) {
                if (isReservedDir(fname) || isHidden(fname)) continue;
                walk(childRel);
            } else if (e.is_regular_file()) {
                if (e.path().extension() != ".json" || isConfigFile(fname) || isHidden(fname))
                    continue;
                ParsedRequestName p = parseRequestFilename(fname);
                if (p.ok && !p.id.empty()) continue;     // đã có id trong tên -> KHÔNG đọc nội dung
                files.push_back(childRel);               // cần migrate (xử lý sau vòng lặp dir)
            }
        }
        for (const auto& childRel : files) {
            try {
                RequestModel m = loadRequest(childRel);  // bảo đảm id sạch (sinh nếu legacy/empty)
                std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
                fs::path src = fs::path(fsutil::join(root_, childRel));
                std::string newName = uniqueEncodedName(src.parent_path(), m.id, m.type, method, m.name);
                if (newName != src.filename().string()) {
                    fs::rename(src, src.parent_path() / newName, ec);
                    if (!ec) ++migrated;
                }
            } catch (...) { /* file lỗi -> bỏ qua, không chặn migrate phần còn lại */ }
        }
    };
    walk("");
    if (migrated) invalidateIdIndex();   // đổi tên file -> idIndex_ (theo relPath) đã lệch
    return migrated;
}

std::string CollectionStore::saveRequest(const std::string& relPath, const RequestModel& m) const {
    invalidateIdIndex();   // có thể rename file (relPath đổi) -> idIndex_ cần dựng lại
    fs::path cur = fs::path(fsutil::join(root_, relPath));
    fs::path dir = cur.parent_path();
    std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
    // Ghi nội dung trước (nguồn chân lý), rồi đồng bộ tên file = cache dẫn xuất (§4).
    fsutil::writeFileAtomic(cur.string(), codec::dumpRequest(m));
    std::string desired = encodeRequestFilename(m.id, m.type, method, m.name);
    if (cur.filename().string() == desired) return relPath;   // tên đã khớp -> xong
    std::string newName = uniqueEncodedName(dir, m.id, m.type, method, m.name);
    fs::path dst = dir / newName;
    std::error_code ec;
    fs::rename(cur, dst, ec);                  // git phát hiện rename qua nội dung giữ nguyên
    if (ec) return relPath;                    // rename lỗi -> giữ path cũ (đã có nội dung)
    return fs::relative(dst, fs::path(root_)).generic_string();
}

std::string CollectionStore::createRequest(const std::string& folderRel, RequestType type,
                                           const std::string& name) const {
    invalidateIdIndex();
    fs::path dir = fs::path(fsutil::join(root_, folderRel));
    fs::create_directories(dir);
    RequestModel m;
    m.id = genId();
    m.name = name;
    m.type = type;
    if (type == RequestType::Http) {
        m.http.method = "GET";
        // 5 header mặc định phổ biến, MẶC ĐỊNH TẮT (enabled=false): gợi ý sẵn,
        // người dùng bật từng cái khi cần (giống cách Postman để header dạng tắt).
        m.http.headers.push_back({"Content-Type", "application/json", false});
        m.http.headers.push_back({"Accept", "*/*", false});
        m.http.headers.push_back({"User-Agent", "deed/0.1", false});
        m.http.headers.push_back({"Accept-Encoding", "gzip, deflate, br", false});
        m.http.headers.push_back({"Connection", "keep-alive", false});
        m.http.body.mode = "none";
    } else {
        m.grpc.methodType = "unary";
        m.grpc.protoSource.mode = "reflection";
        m.grpc.message = "{}";
    }
    std::string method = (type == RequestType::Http) ? m.http.method : std::string();
    fs::path full = dir / uniqueEncodedName(dir, m.id, type, method, name);
    fsutil::writeFileAtomic(full.string(), codec::dumpRequest(m));
    return fs::relative(full, fs::path(root_)).generic_string();
}

std::string CollectionStore::createRequestFromModel(const std::string& folderRel, RequestModel m,
                                                    const std::string& name) const {
    invalidateIdIndex();
    fs::path dir = fs::path(fsutil::join(root_, folderRel));
    fs::create_directories(dir);
    m.id = genId();      // id mới, độc lập với nguồn import
    m.name = name;
    std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
    fs::path full = dir / uniqueEncodedName(dir, m.id, m.type, method, name);
    fsutil::writeFileAtomic(full.string(), codec::dumpRequest(m));
    return fs::relative(full, fs::path(root_)).generic_string();
}

std::string CollectionStore::createFolder(const std::string& parentRel, const std::string& name) const {
    invalidateIdIndex();
    std::string slug = fsutil::slugify(name);
    fs::path dir = fs::path(fsutil::join(fsutil::join(root_, parentRel), slug));
    fs::create_directories(dir);
    return fs::relative(dir, fs::path(root_)).generic_string();
}

std::string CollectionStore::rename(const std::string& relPath, const std::string& newName) const {
    invalidateIdIndex();
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("does not exist: " + relPath);
    if (fs::is_directory(src)) {
        fs::path dst = src.parent_path() / fsutil::slugify(newName);
        fs::rename(src, dst);
        return fs::relative(dst, fs::path(root_)).generic_string();
    }
    // request: cập nhật field name + đổi tên file (GIỮ id cũ, chỉ đổi slug). §2A.
    RequestModel m = loadRequest(relPath);
    m.name = newName;
    std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
    fs::path newFull = src.parent_path() / uniqueEncodedName(src.parent_path(), m.id, m.type, method, newName);
    fsutil::writeFileAtomic(newFull.string(), codec::dumpRequest(m));
    fs::remove(src);
    return fs::relative(newFull, fs::path(root_)).generic_string();
}

std::string CollectionStore::duplicate(const std::string& relPath) const {
    invalidateIdIndex();
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("does not exist: " + relPath);
    if (fs::is_directory(src)) {
        fs::path dst = src.parent_path() / (src.filename().string() + "-copy");
        std::error_code ec;
        fs::copy(src, dst, fs::copy_options::recursive, ec);
        if (ec) throw std::runtime_error("duplicate folder error: " + ec.message());
        return fs::relative(dst, fs::path(root_)).generic_string();
    }
    RequestModel m = loadRequest(relPath);
    m.id = genId();                 // id mới để không trùng
    m.name = m.name + " copy";
    std::string method = (m.type == RequestType::Http) ? m.http.method : std::string();
    fs::path newFull = src.parent_path() / uniqueEncodedName(src.parent_path(), m.id, m.type, method, m.name);
    fsutil::writeFileAtomic(newFull.string(), codec::dumpRequest(m));
    return fs::relative(newFull, fs::path(root_)).generic_string();
}

std::string CollectionStore::move(const std::string& relPath, const std::string& destFolderRel) const {
    invalidateIdIndex();
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("does not exist: " + relPath);
    fs::path destDir = fs::path(fsutil::join(root_, destFolderRel));
    fs::create_directories(destDir);

    // Chặn di chuyển folder vào chính nó / hậu duệ.
    if (fs::is_directory(src)) {
        auto s = fs::weakly_canonical(src);
        auto d = fs::weakly_canonical(destDir);
        auto mismatch = std::mismatch(s.begin(), s.end(), d.begin(), d.end());
        if (mismatch.first == s.end()) throw std::runtime_error("cannot move a folder into itself");
    }
    // Không làm gì nếu đã ở đúng folder.
    if (fs::equivalent(src.parent_path(), destDir)) return relPath;

    std::string stem = src.stem().string();
    std::string ext = src.has_extension() ? src.extension().string() : "";
    fs::path dest = fs::is_directory(src) ? (destDir / src.filename())
                                          : fs::path(uniquePath(destDir, stem, ext));
    fs::rename(src, dest);
    return fs::relative(dest, fs::path(root_)).generic_string();
}

void CollectionStore::remove(const std::string& relPath) const {
    invalidateIdIndex();
    fs::path p = fs::path(fsutil::join(root_, relPath));
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec) throw std::runtime_error("delete error: " + ec.message());
}

void CollectionStore::ensureGitignore() const {
    fs::path gi = fs::path(root_) / ".gitignore";
    std::string content;
    fsutil::readFile(gi.string(), content);
    auto ensureLine = [&](const std::string& line) {
        if (content.find(line) == std::string::npos) {
            if (!content.empty() && content.back() != '\n') content += '\n';
            content += line + "\n";
        }
    };
    ensureLine(".session/");
    ensureLine(".secrets/");
    fsutil::writeFileAtomic(gi.string(), content);
}

} // namespace core
