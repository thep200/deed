// core/importer.hpp — Import from cURL / gRPC (README §8.2, §12.2).
// PARSE only -> RequestModel for the user to review/edit before saving; does NOT write file.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/request/request_model.hpp" // domain RequestModel (importers emit domain natively)

namespace core {

// Importer output — a DOMAIN RequestModel (REFACTOR_SPEC P6: the import use-case is on the domain stack).
// `model` is set only when `ok` (domain RequestModel has no default ctor -> optional).
struct ImportParseResult {
    bool ok = false;
    std::optional<core::domain::RequestModel> model;
    std::vector<std::string> unknown; // unrecognized flags/segments (collected, not a failure)
    std::string error;                // when ok == false
};

class IImporter {
public:
    virtual ~IImporter() = default;
    virtual bool canHandle(const std::string& input) const = 0;
    virtual ImportParseResult parse(const std::string& input) const = 0;
};

// (Export — toCurl — lives in core/infra/export/exporter.hpp.)

// cURL: tokenize the shell command line + map flags -> fields.
class CurlImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportParseResult parse(const std::string& input) const override;
};

// gRPC: (a) grpcurl ... command  (b) host:port/pkg.Service/Method string (grpc://, grpcs://).
class GrpcImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportParseResult parse(const std::string& input) const override;
};

// GraphQL: (a) a raw document starting with query/mutation/subscription, or
//          (b) a cURL command whose JSON body carries a "query" field (-> type=graphql).
class GraphQlImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportParseResult parse(const std::string& input) const override;
};

} // namespace core
