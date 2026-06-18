// request_naming.hpp — quy ước tên file request (LAZY_TREE.md §2, §4).
// Tên file là CACHE DẪN XUẤT của nội dung: cho phép dựng cây mà KHÔNG đọc file.
//   HTTP:  http_<method>_<slug>.json   (method ∈ get|post|put|patch|delete|head|options)
//   gRPC:  grpc_<slug>.json            (KHÔNG có segment method — UI không hiển thị)
// slug có thể chứa cả '_' và '-'; parser tách theo type để không cắt nhầm slug.
#pragma once

#include <string>

#include "core/types.hpp"

namespace core {

// Kết quả parse tên file. ok=false => không đúng grammar (gọi fallback đọc nội dung, §5).
struct ParsedRequestName {
    bool ok = false;
    RequestType type = RequestType::Http;
    std::string method;   // CHỈ http; gRPC để rỗng
    std::string slug;     // phần name-of-request, giữ nguyên '_' và '-' bên trong
};

// Parse "http_get_slug[.json]" / "grpc_slug[.json]". Đuôi .json là tuỳ chọn.
ParsedRequestName parseRequestFilename(const std::string& filename);

// Dựng tên file từ (type, method, tên hiển thị). method bỏ qua khi gRPC.
// -> "http_<method>_<slug>.json" hoặc "grpc_<slug>.json".
std::string encodeRequestFilename(RequestType type, const std::string& method,
                                  const std::string& displayName);

// slug -> tên hiển thị sạch: '_'/'-' thành khoảng trắng, bỏ ký tự đặc biệt,
// gộp khoảng trắng, lowercase, sentence-case (hoa chữ cái đầu). VD: "get-list-user" -> "Get list user".
std::string normalizeDisplayName(const std::string& slug);

} // namespace core
