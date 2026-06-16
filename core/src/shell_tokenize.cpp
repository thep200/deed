#include "shell_tokenize.hpp"

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
            // '\' + newline -> nối dòng (bỏ cả hai); ngược lại escape ký tự kế.
            if (i + 1 < n && (input[i + 1] == '\n' || input[i + 1] == '\r')) {
                i += 2;
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
            if (i < n) ++i; // bỏ nháy đóng
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
            // $'...' ANSI-C quoting: hỗ trợ vài escape phổ biến.
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
