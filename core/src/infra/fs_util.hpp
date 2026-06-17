// fs_util.hpp — tiện ích filesystem nội bộ Core (atomic write, slug, đọc/ghi text).
#pragma once

#include <string>

namespace core::fsutil {

// Đọc toàn bộ file thành string. Trả false nếu không mở được.
bool readFile(const std::string& path, std::string& out);

// Atomic write: ghi ra file tạm cùng thư mục rồi rename đè (README §6.5).
void writeFileAtomic(const std::string& path, const std::string& content);

// Slug hoá tên hiển thị -> tên file an toàn (chữ thường, [a-z0-9-]).
std::string slugify(const std::string& name);

// Tạo thư mục (đệ quy) nếu chưa có.
void ensureDir(const std::string& path);

// Đường app-support của OS cho <appName> (macOS: ~/Library/Application Support/<app>).
std::string appSupportDir(const std::string& appName);

// Nối path tương đối vào gốc (chuẩn hoá tách '/').
std::string join(const std::string& a, const std::string& b);

} // namespace core::fsutil
