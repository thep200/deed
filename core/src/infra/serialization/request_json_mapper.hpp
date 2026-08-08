// The single façade the app/repo layer uses to (de)serialize requests; nlohmann stays in the .cpp.
#pragma once

#include <string>

#include "core/domain/common/result.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::infra {

class RequestJsonMapper {
public:
  domain::Result<domain::RequestModel> fromJson(const std::string &jsonText) const;
  // pretty 2-space; never fails
  std::string toJson(const domain::RequestModel &model) const;
};

} // namespace core::infra
