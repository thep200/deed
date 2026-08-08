#pragma once

#include <string>

namespace core::orderkey {

// Fractional index keys, base62 "0-9A-Za-z" (ASCII order == digit order): a plain byte compare IS the logical order.
// Keys are CASE-SENSITIVE — see collection_store's case-insensitive filename guard (APFS).

// Key strictly between a and b; "" = no neighbour on that side. Requires a < b when both non-empty.
// Throws std::invalid_argument on malformed input — callers reading keys off disk should isValid() first.
std::string between(const std::string& a, const std::string& b);

// Well-formed key: valid head/length, digits in range, fractional part not ending in '0'.
bool isValid(const std::string& key);

} // namespace core::orderkey
