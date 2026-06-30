// core/domain/ports/i_variable_resolver.hpp — substitute {{vars}} in a request (REFACTOR_SPEC §6.3).
#pragma once

#include <string>
#include <unordered_map>

#include "core/domain/common/result.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::domain {

// The active environment's key/value bindings used to resolve {{placeholders}}.
struct VariableScope {
  std::unordered_map<std::string, std::string> values;
};

class IVariableResolver {
public:
  virtual ~IVariableResolver() = default;
  // Return a NEW request with placeholders resolved (immutable); fail if an undefined var is referenced
  // (policy is the impl's choice).
  virtual Result<RequestModel> resolve(const RequestModel &, const VariableScope &) const = 0;
};

} // namespace core::domain
