// SPEC §T7.C — strip comment đầu-dòng (line-leading) trước khi gửi request.
#include "core/codec/comment.hpp"

namespace core::codec {

CommentMarker commentMarkerFor(const std::string& mode) {
    if (mode == "json" || mode == "grpc") return CommentMarker{"//", "", ""};
    if (mode == "graphql") return CommentMarker{"#", "", ""};
    if (mode == "xml") return CommentMarker{"", "<!--", "-->"};
    // text / form-urlencoded / multipart / binary / none -> mặc định '#'.
    return CommentMarker{"#", "", ""};
}

namespace {

// Bỏ các dòng mà ký tự KHÔNG-TRẮNG đầu tiên bắt đầu bằng prefix. Giữ nguyên các
// dòng còn lại (kể cả dòng có prefix nằm giữa, vd URL "https://…").
std::string stripLinePrefix(const std::string& body, const std::string& prefix) {
    std::string out;
    out.reserve(body.size());
    const size_t n = body.size();
    size_t i = 0;
    while (i < n) {
        const size_t lineStart = i;
        const size_t eol = body.find('\n', i);
        const size_t lineEnd = (eol == std::string::npos) ? n : eol;
        size_t j = lineStart;
        while (j < lineEnd && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r')) ++j;
        const bool isComment = (j + prefix.size() <= lineEnd) &&
                               body.compare(j, prefix.size(), prefix) == 0;
        if (!isComment) {
            out.append(body, lineStart, lineEnd - lineStart);
            if (eol != std::string::npos) out.push_back('\n');
        }
        // dòng comment -> bỏ cả dòng lẫn newline của nó.
        i = (eol == std::string::npos) ? n : eol + 1;
    }
    return out;
}

// Gỡ block <!-- … --> (không lồng nhau). Block chưa đóng -> giữ nguyên phần còn lại.
std::string stripBlocks(const std::string& body, const std::string& open, const std::string& close) {
    std::string out;
    out.reserve(body.size());
    const size_t n = body.size();
    size_t i = 0;
    while (i < n) {
        const size_t o = body.find(open, i);
        if (o == std::string::npos) { out.append(body, i, n - i); break; }
        out.append(body, i, o - i);
        const size_t c = body.find(close, o + open.size());
        if (c == std::string::npos) { out.append(body, o, n - o); break; }
        i = c + close.size();
    }
    return out;
}

} // namespace

std::string stripComments(const std::string& body, const std::string& mode) {
    CommentMarker m = commentMarkerFor(mode);
    if (m.hasBlock()) return stripBlocks(body, m.blockOpen, m.blockClose);
    if (m.hasLine()) return stripLinePrefix(body, m.linePrefix);
    return body;
}

} // namespace core::codec
