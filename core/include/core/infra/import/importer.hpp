#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/request/request_model.hpp"

namespace core {

// `model` is set only when `ok` (RequestModel has no default ctor -> optional).
struct ImportParseResult {
    bool ok = false;
    std::optional<core::domain::RequestModel> model;
    std::vector<std::string> unknown; // unrecognized flags/segments (collected, not a failure)
    std::string error;                // when ok == false
};

// Parse only — never writes a file.
class IImporter {
public:
    virtual ~IImporter() = default;
    virtual bool canHandle(const std::string& input) const = 0;
    virtual ImportParseResult parse(const std::string& input) const = 0;
};

class CurlImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportParseResult parse(const std::string& input) const override;
};

// Accepts a `grpcurl ...` command, or a host:port/pkg.Service/Method string (grpc://, grpcs://).
class GrpcImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportParseResult parse(const std::string& input) const override;
};

// Accepts a raw query/mutation/subscription document, or a cURL command whose JSON body carries a "query" field.
class GraphQlImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportParseResult parse(const std::string& input) const override;
};

// Accepts an `ldapsearch ...` command line, or an RFC 4516 URL ldap[s]://host:port/baseDn?attrs?scope?filter.
class LdapImporter : public IImporter {
public:
    bool canHandle(const std::string& input) const override;
    ImportParseResult parse(const std::string& input) const override;
};

} // namespace core
