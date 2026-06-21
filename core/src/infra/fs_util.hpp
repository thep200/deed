// fs_util.hpp — Core-internal filesystem utilities (atomic write, slug, text read/write).
#pragma once

#include <string>

namespace core::fsutil {

// Read the whole file into a string. Returns false if it cannot be opened.
bool readFile(const std::string& path, std::string& out);

// Atomic write: write to a temp file in the same dir then rename over it (README §6.5).
void writeFileAtomic(const std::string& path, const std::string& content);

// Slugify a display name -> safe filename (lowercase, [a-z0-9-]).
std::string slugify(const std::string& name);

// Create the directory (recursively) if it doesn't exist.
void ensureDir(const std::string& path);

// OS app-support path for <appName> (macOS: ~/Library/Application Support/<app>).
std::string appSupportDir(const std::string& appName);

// Join a relative path onto a base (normalizes '/' separators).
std::string join(const std::string& a, const std::string& b);

} // namespace core::fsutil
