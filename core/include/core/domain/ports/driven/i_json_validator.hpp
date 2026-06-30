// core/domain/ports/driven/i_json_validator.hpp — JSON validity check as a port (REFACTOR_SPEC §6.3).
// The domain never parses JSON itself; the saga asks this port to validate a Raw{Json}/gRPC message body.
#pragma once

#include "core/domain/common/result.hpp"
#include "core/domain/values/json_text.hpp"

namespace core::domain {

class IJsonValidator {
public:
  virtual ~IJsonValidator() = default;
  virtual Status validate(const JsonText &) const = 0;
};

} // namespace core::domain
