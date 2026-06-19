// core/codec/comment.hpp — Marker comment theo body mode + strip comment (SPEC §T7).
// Dùng CHUNG giữa UI (toggle comment trong editor) và Core (strip trước khi gửi),
// tránh lệch định nghĩa marker ở hai nơi.
#pragma once

#include <string>

namespace core::codec {

// Marker comment cho một content mode. Mode dùng line-prefix (json/graphql/text…)
// có linePrefix khác rỗng; mode dùng block (xml) có blockOpen/blockClose.
struct CommentMarker {
    std::string linePrefix;   // vd "//", "#"; rỗng nếu mode chỉ dùng block
    std::string blockOpen;    // vd "<!--"; rỗng nếu không dùng block
    std::string blockClose;   // vd "-->"
    bool hasLine() const { return !linePrefix.empty(); }
    bool hasBlock() const { return !blockOpen.empty(); }
};

// Marker theo body mode. mode ∈ {json, text, xml, form-urlencoded, multipart,
// binary, graphql, none, grpc}. gRPC message dùng "//" như JSON.
CommentMarker commentMarkerFor(const std::string& mode);

// Bỏ comment ĐẦU-DÒNG (line-leading) theo mode; KHÔNG đụng marker nằm trong chuỗi
// (vd "https://…") vì chỉ coi là comment khi ký tự không-trắng đầu dòng là marker.
// Mode xml: gỡ block <!-- … -->. Không hỗ trợ comment cuối dòng (trailing) — đánh đổi
// lấy an toàn (SPEC §T7.C).
std::string stripComments(const std::string& body, const std::string& mode);

} // namespace core::codec
