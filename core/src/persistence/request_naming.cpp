#include "core/persistence/request_naming.hpp"

#include <cctype>

#include "infra/fs_util.hpp"

namespace core {

namespace {

// Bỏ đuôi ".json" (không phân biệt hoa thường) nếu có.
std::string stripJsonExt(const std::string& name) {
    const std::string ext = ".json";
    if (name.size() >= ext.size()) {
        std::string tail = name.substr(name.size() - ext.size());
        for (auto& c : tail) c = static_cast<char>(std::tolower((unsigned char)c));
        if (tail == ext) return name.substr(0, name.size() - ext.size());
    }
    return name;
}

} // namespace

namespace {
// Parse phần "http_<method>_<slug>" / "grpc_<slug>" (sau khi đã tách id). Trả ok + điền type/method/slug.
bool parseTypeRest(const std::string& rest, ParsedRequestName& out) {
    std::size_t sep = rest.find('_');
    if (sep == std::string::npos) return false;
    std::string type = rest.substr(0, sep);
    if (type == "grpc") {
        out.type = RequestType::Grpc;
        out.slug = rest.substr(sep + 1);            // toàn bộ còn lại = slug
        out.method.clear();
        return !out.slug.empty();
    }
    if (type == "http") {
        std::size_t methodSep = rest.find('_', sep + 1);   // [http, method, slugRest]
        if (methodSep == std::string::npos) return false;
        out.type = RequestType::Http;
        out.method = rest.substr(sep + 1, methodSep - (sep + 1));
        out.slug = rest.substr(methodSep + 1);      // slug giữ nguyên '_'
        return !out.method.empty() && !out.slug.empty();
    }
    return false;
}
} // namespace

bool isValidFileId(const std::string& id) {
    if (id.empty()) return false;
    for (unsigned char c : id)
        if (!std::islower(c) && !std::isdigit(c)) return false;   // chỉ [a-z0-9]
    return true;
}

ParsedRequestName parseRequestFilename(const std::string& filename) {
    ParsedRequestName out;
    std::string base = stripJsonExt(filename);

    std::size_t firstSep = base.find('_');
    if (firstSep == std::string::npos) return out;   // không đúng grammar -> ok=false

    std::string firstTok = base.substr(0, firstSep);
    // Dạng CŨ (chưa có id): token đầu chính là type -> id rỗng, parse toàn chuỗi theo type.
    if (firstTok == "http" || firstTok == "grpc") {
        out.id.clear();
        out.ok = parseTypeRest(base, out);
        return out;
    }
    // Dạng MỚI: token đầu = id, phần còn lại theo type.
    out.id = firstTok;
    std::string rest = base.substr(firstSep + 1);
    out.ok = isValidFileId(out.id) && parseTypeRest(rest, out);
    return out;
}

std::string encodeRequestFilename(const std::string& id, RequestType type,
                                  const std::string& method, const std::string& displayName) {
    std::string slug = fsutil::slugify(displayName);   // [a-z0-9-], '-' thay khoảng trắng
    std::string prefix = id + "_";                     // id luôn nằm đầu (caller bảo đảm id hợp lệ)
    if (type == RequestType::Grpc) return prefix + "grpc_" + slug + ".json";   // KHÔNG có method
    std::string m;
    for (unsigned char c : method) m += static_cast<char>(std::tolower(c));
    if (m.empty()) m = "get";
    return prefix + "http_" + m + "_" + slug + ".json";
}

std::string normalizeDisplayName(const std::string& slug) {
    std::string s;
    s.reserve(slug.size());
    bool prevSpace = false;
    for (unsigned char c : slug) {
        if (c == '_' || c == '-' || std::isspace(c)) {
            if (!prevSpace && !s.empty()) { s += ' '; prevSpace = true; }
        } else if (std::isalnum(c)) {
            s += static_cast<char>(std::tolower(c));
            prevSpace = false;
        }
        // ký tự đặc biệt khác -> bỏ
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();   // trim đuôi
    if (!s.empty()) s[0] = static_cast<char>(std::toupper((unsigned char)s[0]));  // sentence-case
    return s;
}

} // namespace core
