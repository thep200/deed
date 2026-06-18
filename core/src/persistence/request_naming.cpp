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

ParsedRequestName parseRequestFilename(const std::string& filename) {
    ParsedRequestName out;
    std::string base = stripJsonExt(filename);

    std::size_t firstSep = base.find('_');
    if (firstSep == std::string::npos) return out;   // không đúng grammar -> ok=false

    std::string type = base.substr(0, firstSep);
    if (type == "grpc") {
        out.type = RequestType::Grpc;
        out.slug = base.substr(firstSep + 1);        // toàn bộ phần còn lại = slug
        out.method.clear();
        out.ok = !out.slug.empty();
        return out;
    }
    if (type == "http") {
        // tách TỐI ĐA 3 phần: [http, method, slugRest] — slug giữ nguyên cả '_'.
        std::size_t methodSep = base.find('_', firstSep + 1);
        if (methodSep == std::string::npos) return out;  // thiếu slug
        out.type = RequestType::Http;
        out.method = base.substr(firstSep + 1, methodSep - (firstSep + 1));
        out.slug = base.substr(methodSep + 1);
        out.ok = !out.method.empty() && !out.slug.empty();
        return out;
    }
    return out;   // type lạ -> ok=false (fallback đọc nội dung)
}

std::string encodeRequestFilename(RequestType type, const std::string& method,
                                  const std::string& displayName) {
    std::string slug = fsutil::slugify(displayName);   // [a-z0-9-], '-' thay khoảng trắng
    if (type == RequestType::Grpc) return "grpc_" + slug + ".json";   // KHÔNG có method
    std::string m;
    for (unsigned char c : method) m += static_cast<char>(std::tolower(c));
    if (m.empty()) m = "get";
    return "http_" + m + "_" + slug + ".json";
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
