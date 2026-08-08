// On-disk schema keeps the legacy field names so existing cache files still load.
#pragma once

#include <string>

#include "core/infra/cache/cache.hpp"

namespace core::cachecodec {

std::string toJson(const ResponseRecord& rec);

// Throws on malformed input — callers already catch.
ResponseRecord fromJson(const std::string& text);

} // namespace core::cachecodec
