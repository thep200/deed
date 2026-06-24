// core/importer.hpp — Import from cURL / gRPC (README §8.2, §12.2).
// PARSE only -> RequestModel for the user to review/edit before saving; does NOT write file.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace core {

struct ImportResult {
    bool ok = false;
    RequestModel model;
    std::vector<std::string> unknown; // unrecognized flags/segments (collected, not a failure)
    std::string error;                // when ok == false
};

class IImporter {
public:
    virtual ~IImporter() = default;
    virtual bool canHandle(const std::string& input) const = 0;
    virtual ImportResult parse(const std::string& input) const = 0;
};

// Export current request as a cURL (HTTP) / grpcurl (gRPC) command. Pass a RESOLVED model.
std::string toCurl(const RequestModel& resolved);

// cURL: tokenize the shell command line + map flags -> fields.
class CurlImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportResult parse(const std::string& input) const override;
};

// gRPC: (a) grpcurl ... command  (b) host:port/pkg.Service/Method string (grpc://, grpcs://).
class GrpcImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportResult parse(const std::string& input) const override;
};

// GraphQL: (a) a raw document starting with query/mutation/subscription, or
//          (b) a cURL command whose JSON body carries a "query" field (-> type=graphql).
class GraphQlImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportResult parse(const std::string& input) const override;
};

} // namespace core
