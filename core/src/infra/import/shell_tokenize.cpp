#include "infra/import/shell_tokenize.hpp"

#include <algorithm>
#include <cctype>

namespace core {

namespace {

// Scanners take the index just after the opening quote and return the index past the closing one (n if unterminated).

size_t scanSingleQuote(const std::string& in, size_t i, size_t n, std::string& cur) {
    while (i < n && in[i] != '\'') cur += in[i++];
    return i < n ? i + 1 : i; // drop closing quote
}

size_t scanDoubleQuote(const std::string& in, size_t i, size_t n, std::string& cur) {
    while (i < n && in[i] != '"') {
        if (in[i] == '\\' && i + 1 < n) { cur += in[i + 1]; i += 2; }
        else cur += in[i++];
    }
    return i < n ? i + 1 : i;
}

// $'...' ANSI-C quoting escape: a few common ones; everything else (incl. '\\' and '\'') stays literal.
char ansiCEscape(char e) {
    switch (e) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        default: return e;
    }
}

size_t scanAnsiCQuote(const std::string& in, size_t i, size_t n, std::string& cur) {
    while (i < n && in[i] != '\'') {
        if (in[i] == '\\' && i + 1 < n) { cur += ansiCEscape(in[i + 1]); i += 2; }
        else cur += in[i++];
    }
    return i < n ? i + 1 : i;
}

// Backslash before whitespace is a line continuation (dropped); otherwise it escapes the next char.
size_t handleBackslash(const std::string& in, size_t i, size_t n, std::string& cur, bool& inTok) {
    if (i + 1 < n && (in[i + 1] == '\n' || in[i + 1] == '\r' || in[i + 1] == ' ' || in[i + 1] == '\t'))
        return i + 1;
    if (i + 1 < n) { cur += in[i + 1]; inTok = true; return i + 2; }
    return i + 1;
}

} // namespace

std::vector<std::string> shellTokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inTok = false;
    size_t i = 0;
    const size_t n = input.size();

    auto push = [&] {
        if (inTok) { tokens.push_back(cur); cur.clear(); inTok = false; }
    };

    while (i < n) {
        char c = input[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { push(); ++i; continue; }
        if (c == '\\') { i = handleBackslash(input, i, n, cur, inTok); continue; }
        if (c == '\'') { inTok = true; i = scanSingleQuote(input, i + 1, n, cur); continue; }
        if (c == '"') { inTok = true; i = scanDoubleQuote(input, i + 1, n, cur); continue; }
        if (c == '$' && i + 1 < n && input[i + 1] == '\'') {
            inTok = true; i = scanAnsiCQuote(input, i + 2, n, cur); continue;
        }
        cur += c;
        inTok = true;
        ++i;
    }
    push();
    return tokens;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace core
