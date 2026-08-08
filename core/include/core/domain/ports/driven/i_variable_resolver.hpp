#pragma once

#include <string>
#include <unordered_map>

#include "core/domain/common/result.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::domain {

struct VariableScope {
  std::unordered_map<std::string, std::string> values;
};

class IVariableResolver {
public:
  virtual ~IVariableResolver() = default;
  // Returns a NEW resolved request; whether an undefined var fails is the impl's policy.
  virtual Result<RequestModel> resolve(const RequestModel &, const VariableScope &) const = 0;
};

} // namespace core::domain
