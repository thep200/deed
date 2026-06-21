// url_util — shared URL utilities (percent-encoding decode, split query after '?').
// Header-only (inline) -> usable in both import_export and sending without adding a CMake source.
#pragma once

#include <string>
#include <vector>

#include "core/types.hpp"

namespace core::urlutil {

// Decode percent-encoding: %XX -> byte, '+' -> space. Invalid sequences kept as-is.
inline std::string urlDecode(const std::string& s) {
    auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>(hi * 16 + lo); i += 2; }
            else out += c;
        } else {
            out += c;
        }
    }
    return out;
}

// Split the query (after '?') off the url: url becomes RAW (drops '?...'), k=v pairs are
// DECODED and pushed into params (enabled=true). No '?' -> no-op.
inline void splitUrlQuery(std::string& url, std::vector<KeyValue>& params) {
    size_t q = url.find('?');
    if (q == std::string::npos) return;
    std::string query = url.substr(q + 1);
    url = url.substr(0, q);
    size_t hash = query.find('#');           // drop fragment if mixed in
    if (hash != std::string::npos) query = query.substr(0, hash);
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string seg = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!seg.empty()) {
            size_t eq = seg.find('=');
            KeyValue kv;
            kv.enabled = true;
            if (eq == std::string::npos) {
                kv.key = urlDecode(seg);
            } else {
                kv.key = urlDecode(seg.substr(0, eq));
                kv.value = urlDecode(seg.substr(eq + 1));
            }
            params.push_back(kv);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
}

} // namespace core::urlutil
