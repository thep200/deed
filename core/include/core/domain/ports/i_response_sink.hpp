// core/domain/ports/i_response_sink.hpp — where a sender pushes lifecycle events (REFACTOR_SPEC §6.3).
#pragma once

#include "core/domain/response/response_event.hpp"

namespace core::domain {

class IResponseSink {
public:
  virtual ~IResponseSink() = default;
  virtual void emit(const ResponseEvent &ev) = 0;
};

} // namespace core::domain
