// request_naming.hpp — quy ước tên file request (new_format.file.md §2A; cập nhật LAZY_TREE §2).
// Tên file là CACHE DẪN XUẤT của nội dung: cho phép dựng cây + lấy id mà KHÔNG đọc file.
//   HTTP:  <id>_http_<method>_<slug>.json   (method ∈ get|post|put|patch|delete|head|options)
//   gRPC:  <id>_grpc_<slug>.json            (KHÔNG có segment method — UI không hiển thị)
// id: [a-z0-9]+ (KHÔNG chứa '_'), duy nhất/ổn định; KHÔNG hiển thị trên UI.
// slug có thể chứa cả '_' và '-'; parser tách token đầu = id, phần còn lại theo type.
// BACK-COMPAT: file cũ không có id (token đầu là 'http'/'grpc') -> parse được, id rỗng (migrate sau).
#pragma once

#include <string>

#include "core/types.hpp"

namespace core {

// Kết quả parse tên file. ok=false => không đúng grammar (gọi fallback đọc nội dung, §5).
struct ParsedRequestName {
    bool ok = false;
    std::string id;       // token đầu (rỗng nếu file cũ chưa có id)
    RequestType type = RequestType::Http;
    std::string method;   // CHỈ http; gRPC để rỗng
    std::string slug;     // phần name-of-request, giữ nguyên '_' và '-' bên trong
};

// Parse "<id>_http_get_slug[.json]" / "<id>_grpc_slug[.json]" (và dạng cũ không id). .json tuỳ chọn.
ParsedRequestName parseRequestFilename(const std::string& filename);

// Dựng tên file từ (id, type, method, tên hiển thị). method bỏ qua khi gRPC.
// -> "<id>_http_<method>_<slug>.json" hoặc "<id>_grpc_<slug>.json".
std::string encodeRequestFilename(const std::string& id, RequestType type,
                                  const std::string& method, const std::string& displayName);

// True nếu chuỗi hợp lệ làm id trong TÊN FILE (chỉ [a-z0-9], không rỗng, không '_').
bool isValidFileId(const std::string& id);

// slug -> tên hiển thị sạch: '_'/'-' thành khoảng trắng, bỏ ký tự đặc biệt,
// gộp khoảng trắng, lowercase, sentence-case (hoa chữ cái đầu). VD: "get-list-user" -> "Get list user".
std::string normalizeDisplayName(const std::string& slug);

} // namespace core
