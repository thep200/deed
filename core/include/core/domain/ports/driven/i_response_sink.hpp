#pragma once

#include "core/domain/response/response_event.hpp"

namespace core::domain {

class IResponseSink {
public:
  virtual ~IResponseSink() = default;
  virtual void emit(const ResponseEvent &ev) = 0;
};

} // namespace core::domain
