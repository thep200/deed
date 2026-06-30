// core/infra/export/exporter.hpp — export a request to a command line (README §8.2, §12.2).
// Split out of importer.hpp so import/ and export/ are separate concerns (the export side has no
// dependency on the IImporter parsing machinery).
#pragma once

#include <string>

#include "core/domain/request/request_model.hpp" // domain RequestModel (toCurl takes a resolved model)

namespace core {

// Export current request as a cURL (HTTP) / grpcurl (gRPC) command. Pass a RESOLVED domain model.
std::string toCurl(const core::domain::RequestModel& resolved);

} // namespace core
