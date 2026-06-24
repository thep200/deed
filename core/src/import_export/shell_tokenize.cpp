#include "import_export/shell_tokenize.hpp"

namespace core {

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
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            push();
            ++i;
            continue;
        }
        if (c == '\\') {
            // '\' + whitespace -> line continuation: drop the backslash, let the whitespace separate.
            // (A pasted multi-line command flattened by a single-line field turns "\\\n" into "\ ", so a
            // backslash before any space/tab/newline is always a continuation here, never an escaped space.)
            if (i + 1 < n && (input[i + 1] == '\n' || input[i + 1] == '\r' ||
                              input[i + 1] == ' ' || input[i + 1] == '\t')) {
                ++i;          // drop only the backslash; the whitespace is handled as a separator next
                continue;
            }
            if (i + 1 < n) { cur += input[i + 1]; inTok = true; i += 2; continue; }
            ++i;
            continue;
        }
        if (c == '\'') {
            inTok = true;
            ++i;
            while (i < n && input[i] != '\'') cur += input[i++];
            if (i < n) ++i; // drop closing quote
            continue;
        }
        if (c == '"') {
            inTok = true;
            ++i;
            while (i < n && input[i] != '"') {
                if (input[i] == '\\' && i + 1 < n) { cur += input[i + 1]; i += 2; }
                else cur += input[i++];
            }
            if (i < n) ++i;
            continue;
        }
        if (c == '$' && i + 1 < n && input[i + 1] == '\'') {
            // $'...' ANSI-C quoting: support a few common escapes.
            inTok = true;
            i += 2;
            while (i < n && input[i] != '\'') {
                if (input[i] == '\\' && i + 1 < n) {
                    char e = input[i + 1];
                    switch (e) {
                        case 'n': cur += '\n'; break;
                        case 't': cur += '\t'; break;
                        case 'r': cur += '\r'; break;
                        case '\\': cur += '\\'; break;
                        case '\'': cur += '\''; break;
                        default: cur += e; break;
                    }
                    i += 2;
                } else {
                    cur += input[i++];
                }
            }
            if (i < n) ++i;
            continue;
        }
        cur += c;
        inTok = true;
        ++i;
    }
    push();
    return tokens;
}

} // namespace core
