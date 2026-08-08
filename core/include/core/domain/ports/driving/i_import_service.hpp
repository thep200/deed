#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::domain {

enum class ImportKind { Curl, Grpcurl, GraphQl, Ldap };

// `unknown` = unrecognized flags/segments the importer skipped — surfaced to the user, not a failure.
struct ImportOutcome {
  RequestModel model;
  std::vector<std::string> unknown;
};

class IImportService {
public:
  virtual ~IImportService() = default;
  // Best-effort classification of pasted text (nullopt = not an importable command).
  virtual std::optional<ImportKind> detect(const std::string &text) const = 0;
  virtual Result<ImportOutcome> import(const std::string &text, ImportKind kind) const = 0;
};

} // namespace core::domain
