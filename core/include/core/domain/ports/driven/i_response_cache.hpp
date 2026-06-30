// core/domain/ports/driven/i_response_cache.hpp — store/fetch the last response per request (REFACTOR_SPEC §6.3/§8.4).
#pragma once

#include <optional>

#include "core/domain/request/request_id.hpp"
#include "core/domain/response/api_response.hpp"

namespace core::domain {

class IResponseCache {
public:
  virtual ~IResponseCache() = default;
  virtual void put(const RequestId &id, const ApiResponse &response) = 0;
  virtual std::optional<ApiResponse> get(const RequestId &id) const = 0;
};

} // namespace core::domain
