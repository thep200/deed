// core/importer.hpp — Import từ cURL / gRPC (README §8.2, §12.2).
// Chỉ PARSE -> RequestModel để user xem/sửa rồi mới lưu; KHÔNG tự ghi file.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace core {

struct ImportResult {
    bool ok = false;
    RequestModel model;
    std::vector<std::string> unknown; // cờ/đoạn không nhận diện được (gom lại, không fail)
    std::string error;                // khi ok == false
};

class IImporter {
public:
    virtual ~IImporter() = default;
    virtual bool canHandle(const std::string& input) const = 0;
    virtual ImportResult parse(const std::string& input) const = 0;
};

// cURL: tokenizing dòng lệnh shell + map cờ -> field.
class CurlImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportResult parse(const std::string& input) const override;
};

// gRPC: (a) lệnh grpcurl ...  (b) chuỗi host:port/pkg.Service/Method (grpc://, grpcs://).
class GrpcImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportResult parse(const std::string& input) const override;
};

} // namespace core
