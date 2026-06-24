#include "infra/fs_util.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

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

    // Unique temp name per writer (H6): two concurrent writers to the same path must not share a ".tmp"
    // and clobber each other. pid + a process-local counter makes it unique.
    static std::atomic<unsigned long long> ctr{0};
    long long pid =
#ifndef _WIN32
        static_cast<long long>(::getpid());
#else
        0;
#endif
    fs::path tmp = target;
    tmp += "." + std::to_string(pid) + "." + std::to_string(ctr.fetch_add(1)) + ".tmp";

    auto cleanup = [&] { std::error_code e; fs::remove(tmp, e); };

#ifndef _WIN32
    // POSIX: write + fsync the data before rename, then fsync the parent dir so a crash after rename can't
    // leave a zero-length/stale file (H6).
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw std::runtime_error("cannot open temp file: " + tmp.string());
    const char* p = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd); cleanup();
            throw std::runtime_error("failed to write temp file: " + tmp.string());
        }
        p += n; left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); cleanup(); throw std::runtime_error("fsync temp failed: " + tmp.string()); }
    ::close(fd);

    std::error_code ec;
    fs::rename(tmp, target, ec);   // atomic on the same filesystem
    if (ec) {
        // Cross-FS (or other) failure: copy onto the target, then drop the temp. Clean up on every path (L6).
        std::error_code ec2;
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec2);
        cleanup();
        if (ec2) throw std::runtime_error("atomic rename failed: " + ec.message());
    }
    if (target.has_parent_path()) {
        int dfd = ::open(target.parent_path().c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                                       | O_DIRECTORY
#endif
        );
        if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }   // make the new dirent durable
    }
#else
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open temp file: " + tmp.string());
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) { cleanup(); throw std::runtime_error("failed to write temp file: " + tmp.string()); }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2;
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec2);
        cleanup();
        if (ec2) throw std::runtime_error("atomic rename failed: " + ec.message());
    }
#endif
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
        // other chars -> drop
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
