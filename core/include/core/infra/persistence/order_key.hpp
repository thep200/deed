// order_key.hpp — fractional index keys used as the collection ordering prefix.
// Base62 "0-9A-Za-z" (ASCII order == digit order), so a plain byte compare of two keys is the
// logical order. Inserting between two neighbours always yields a NEW key without touching them
// -> a reorder renames exactly ONE entry, never the whole level.
// Keys are CASE-SENSITIVE; see collection_store's case-insensitive filename guard (APFS).
#pragma once

#include <string>

namespace core::orderkey {

// Key strictly between a and b. "" = no neighbour on that side (start/end of the level).
// Requires a < b when both are non-empty. Throws std::invalid_argument on malformed input —
// callers reading keys off disk should isValid() first and treat a bad key as "".
std::string between(const std::string& a, const std::string& b);

// Well-formed key: valid head/length, digits in range, fractional part not ending in '0'.
bool isValid(const std::string& key);

} // namespace core::orderkey
