#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>

#include "core/stores.hpp"
#include "fs_util.hpp"
#include "json_codec.hpp"

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

// Sinh id ngẫu nhiên nhẹ (không thư viện ngoài): RNG seed MỘT LẦN rồi tiến trạng thái
// mỗi lần gọi -> không trùng dù gọi liên tiếp trong cùng mili-giây.
std::string genId() {
    static std::mutex mu;
    static std::mt19937_64 rng([] {
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        s ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return s;
    }());
    static const char* alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford-ish
    std::lock_guard<std::mutex> lk(mu);
    std::string s = "req_";
    for (int i = 0; i < 12; ++i) s += alphabet[rng() % 32];
    return s;
}

std::string uniquePath(const fs::path& dir, const std::string& slug, const std::string& ext) {
    fs::path cand = dir / (slug + ext);
    if (!fs::exists(cand)) return cand.string();
    for (int i = 2; i < 10000; ++i) {
        fs::path c = dir / (slug + "-" + std::to_string(i) + ext);
        if (!fs::exists(c)) return c.string();
    }
    throw std::runtime_error("không tìm được tên file duy nhất cho: " + slug);
}

} // namespace

CollectionStore::CollectionStore(std::string root) : root_(std::move(root)) {}
void CollectionStore::setRoot(std::string root) { root_ = std::move(root); }

TreeNode CollectionStore::scanTree() const {
    std::function<TreeNode(const fs::path&, const std::string&)> walk =
        [&](const fs::path& dir, const std::string& rel) -> TreeNode {
        TreeNode node;
        node.isFolder = true;
        node.relPath = rel;
        node.name = rel.empty() ? fs::path(root_).filename().string()
                                : fs::path(rel).filename().string();
        std::error_code ec;
        std::vector<fs::directory_entry> entries;
        for (const auto& e : fs::directory_iterator(dir, ec)) entries.push_back(e);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            if (a.is_directory() != b.is_directory()) return a.is_directory(); // folder lên trước
            return a.path().filename().string() < b.path().filename().string();
        });
        for (const auto& e : entries) {
            std::string fname = e.path().filename().string();
            std::string childRel = rel.empty() ? fname : rel + "/" + fname;
            if (e.is_directory()) {
                if (isReservedDir(fname) || isHidden(fname)) continue;
                node.children.push_back(walk(e.path(), childRel));
            } else if (e.is_regular_file()) {
                if (e.path().extension() != ".json") continue;
                if (isConfigFile(fname) || isHidden(fname)) continue;
                TreeNode leaf;
                leaf.isFolder = false;
                leaf.relPath = childRel;
                leaf.name = fname; // fallback; ghi đè bằng field name nếu đọc được
                // Lazy metadata: đọc name/type/method (đọc file nhỏ — chấp nhận).
                std::string txt;
                if (fsutil::readFile(e.path().string(), txt)) {
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
                            leaf.methodOrType = j.value("grpc", codec::json::object()).value("methodType", "unary");
                    } catch (...) { /* file lỗi -> vẫn hiện tên file */ }
                }
                node.children.push_back(std::move(leaf));
            }
        }
        return node;
    };
    return walk(fs::path(root_), "");
}

RequestModel CollectionStore::loadRequest(const std::string& relPath) const {
    std::string txt;
    if (!fsutil::readFile(fsutil::join(root_, relPath), txt))
        throw std::runtime_error("không đọc được request: " + relPath);
    RequestModel m = codec::requestFromJson(codec::json::parse(txt));
    if (m.id.empty()) {                 // file cũ chưa có id -> gán + ghi lại (migrate 1 lần)
        m.id = genId();
        try { saveRequest(relPath, m); } catch (...) {}
    }
    return m;
}

std::string CollectionStore::findRelPathById(const std::string& id) const {
    if (id.empty()) return "";
    std::string found;
    std::function<void(const TreeNode&)> walk = [&](const TreeNode& n) {
        if (!found.empty()) return;
        if (!n.isFolder && n.id == id) { found = n.relPath; return; }
        for (const auto& c : n.children) walk(c);
    };
    walk(scanTree());
    return found;
}

void CollectionStore::saveRequest(const std::string& relPath, const RequestModel& m) const {
    fsutil::writeFileAtomic(fsutil::join(root_, relPath), codec::dumpRequest(m));
}

std::string CollectionStore::createRequest(const std::string& folderRel, RequestType type,
                                           const std::string& name) const {
    fs::path dir = fs::path(fsutil::join(root_, folderRel));
    fs::create_directories(dir);
    std::string slug = fsutil::slugify(name);
    std::string full = uniquePath(dir, slug, ".json");
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
    fsutil::writeFileAtomic(full, codec::dumpRequest(m));
    return fs::relative(full, fs::path(root_)).generic_string();
}

std::string CollectionStore::createFolder(const std::string& parentRel, const std::string& name) const {
    std::string slug = fsutil::slugify(name);
    fs::path dir = fs::path(fsutil::join(fsutil::join(root_, parentRel), slug));
    fs::create_directories(dir);
    return fs::relative(dir, fs::path(root_)).generic_string();
}

std::string CollectionStore::rename(const std::string& relPath, const std::string& newName) const {
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("không tồn tại: " + relPath);
    if (fs::is_directory(src)) {
        fs::path dst = src.parent_path() / fsutil::slugify(newName);
        fs::rename(src, dst);
        return fs::relative(dst, fs::path(root_)).generic_string();
    }
    // request: cập nhật field name + đổi slug file
    RequestModel m = loadRequest(relPath);
    m.name = newName;
    std::string newFull = uniquePath(src.parent_path(), fsutil::slugify(newName), ".json");
    fsutil::writeFileAtomic(newFull, codec::dumpRequest(m));
    fs::remove(src);
    return fs::relative(fs::path(newFull), fs::path(root_)).generic_string();
}

std::string CollectionStore::duplicate(const std::string& relPath) const {
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("không tồn tại: " + relPath);
    if (fs::is_directory(src)) {
        fs::path dst = src.parent_path() / (src.filename().string() + "-copy");
        std::error_code ec;
        fs::copy(src, dst, fs::copy_options::recursive, ec);
        if (ec) throw std::runtime_error("duplicate folder lỗi: " + ec.message());
        return fs::relative(dst, fs::path(root_)).generic_string();
    }
    RequestModel m = loadRequest(relPath);
    m.id = genId();                 // id mới để không trùng
    m.name = m.name + " copy";
    std::string slug = src.stem().string() + "-copy";
    std::string newFull = uniquePath(src.parent_path(), slug, ".json");
    fsutil::writeFileAtomic(newFull, codec::dumpRequest(m));
    return fs::relative(fs::path(newFull), fs::path(root_)).generic_string();
}

std::string CollectionStore::move(const std::string& relPath, const std::string& destFolderRel) const {
    fs::path src = fs::path(fsutil::join(root_, relPath));
    if (!fs::exists(src)) throw std::runtime_error("không tồn tại: " + relPath);
    fs::path destDir = fs::path(fsutil::join(root_, destFolderRel));
    fs::create_directories(destDir);

    // Chặn di chuyển folder vào chính nó / hậu duệ.
    if (fs::is_directory(src)) {
        auto s = fs::weakly_canonical(src);
        auto d = fs::weakly_canonical(destDir);
        auto mismatch = std::mismatch(s.begin(), s.end(), d.begin(), d.end());
        if (mismatch.first == s.end()) throw std::runtime_error("không thể di chuyển vào chính nó");
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
    fs::path p = fs::path(fsutil::join(root_, relPath));
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec) throw std::runtime_error("xoá lỗi: " + ec.message());
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
