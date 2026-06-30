// core/src/infra/import_export/import_service.hpp — IImportService over the existing importers
// (REFACTOR_SPEC P6). Wraps CurlImporter/GrpcImporter/GraphQlImporter + the request bridge.
#pragma once

#include "core/domain/ports/i_import_service.hpp"
#include "core/import_export/importer.hpp"

namespace core::infra {

class ImportService final : public domain::IImportService {
public:
  std::optional<domain::ImportKind> detect(const std::string &text) const override;
  domain::Result<domain::ImportOutcome> import(const std::string &text,
                                               domain::ImportKind kind) const override;

private:
  core::CurlImporter curl_;
  core::GrpcImporter grpc_;
  core::GraphQlImporter graphql_;
};

} // namespace core::infra
