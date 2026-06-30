// core/src/infra/serialization/request_json_mapper.hpp — JSON <-> domain::RequestModel (REFACTOR_SPEC §8.1).
// The ONLY façade the app/repository layer uses to (de)serialize requests. NATIVE: parses/writes the on-disk
// JSON schema DIRECTLY into the domain aggregate via core::serial (no legacy json_codec / request_bridge),
// keeping the golden rule (nlohmann only in this .cpp).
#pragma once

#include <string>

#include "core/domain/common/result.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::infra {

class RequestJsonMapper {
public:
  // Parse a request JSON document (the file format) into the domain aggregate.
  domain::Result<domain::RequestModel> fromJson(const std::string &jsonText) const;
  // Serialize the domain aggregate to the on-disk JSON document (pretty, 2-space) — never fails.
  std::string toJson(const domain::RequestModel &model) const;
};

} // namespace core::infra
