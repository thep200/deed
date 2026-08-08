#pragma once

#include <string>

namespace core::fsutil {

bool readFile(const std::string& path, std::string& out);

// Temp file in the same dir, then rename over the target.
void writeFileAtomic(const std::string& path, const std::string& content);

// lowercase [a-z0-9-]
std::string slugify(const std::string& name);

void ensureDir(const std::string& path);

std::string appSupportDir(const std::string& appName);

std::string join(const std::string& a, const std::string& b);

} // namespace core::fsutil
