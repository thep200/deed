// core/domain/ports/driving/i_import_service.hpp — detect + parse a pasted command into a domain RequestModel
// (REFACTOR_SPEC P6, replaces the UI's direct use of Engine import). Pure: no network, no JSON outward.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::domain {

enum class ImportKind { Curl, Grpcurl, GraphQl };

// Result of a successful import: the parsed request plus any unrecognized flags/segments the
// importer skipped (surfaced to the user as "skipped: ..." — collected, not a failure).
struct ImportOutcome {
  RequestModel model;
  std::vector<std::string> unknown;
};

class IImportService {
public:
  virtual ~IImportService() = default;
  // Best-effort classification of pasted text (nullopt = not an importable command).
  virtual std::optional<ImportKind> detect(const std::string &text) const = 0;
  // Parse into a domain request; fail (Result) on parse error. On success carries the skipped flags.
  virtual Result<ImportOutcome> import(const std::string &text, ImportKind kind) const = 0;
};

} // namespace core::domain
