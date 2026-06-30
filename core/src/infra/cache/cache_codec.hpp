// cache_codec.hpp — serialize the disk-cache ResponseRecord <-> JSON (domain ApiResponse/ApiError).
// INTERNAL (core/src): nlohmann is used in the .cpp only. Replaces the legacy json_codec ResponseRecord
// path (REFACTOR_SPEC D — the cache speaks domain types now). The on-disk schema is unchanged for the
// common fields (statusCode/headers/cookies/body/elapsed/error/meta) so existing cache files still load.
#pragma once

#include <string>

#include "core/infra/cache/cache.hpp"

namespace core::cachecodec {

// Pretty-free compact dump (cache files are machine-only).
std::string toJson(const ResponseRecord& rec);

// Parse; throws (nlohmann/std::exception) on malformed input — callers already catch.
ResponseRecord fromJson(const std::string& text);

} // namespace core::cachecodec
