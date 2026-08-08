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
