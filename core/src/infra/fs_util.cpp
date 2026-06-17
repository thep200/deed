#include "infra/fs_util.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace core::fsutil {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void writeFileAtomic(const std::string& path, const std::string& content) {
    fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path());
    }
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open temp file: " + tmp.string());
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) throw std::runtime_error("failed to write temp file: " + tmp.string());
    }
    std::error_code ec;
    fs::rename(tmp, target, ec); // atomic trên cùng filesystem
    if (ec) {
        // fallback: copy đè rồi xoá tạm
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) throw std::runtime_error("atomic rename failed: " + ec.message());
    }
}

std::string slugify(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    bool prevDash = false;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
            prevDash = false;
        } else if (c == '-' || c == '_' || std::isspace(c) || c == '/' || c == '.') {
            if (!prevDash && !out.empty()) {
                out += '-';
                prevDash = true;
            }
        }
        // ký tự khác -> bỏ
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "untitled";
    return out;
}

void ensureDir(const std::string& path) {
    fs::create_directories(path);
}

std::string appSupportDir(const std::string& appName) {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : ".";
#if defined(__APPLE__)
    fs::path p = fs::path(base) / "Library" / "Application Support" / appName;
#elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    fs::path p = fs::path(appdata ? appdata : base) / appName;
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path p = xdg ? (fs::path(xdg) / appName)
                     : (fs::path(base) / ".config" / appName);
#endif
    return p.string();
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return (fs::path(a) / b).lexically_normal().string();
}

} // namespace core::fsutil
