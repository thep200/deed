#pragma once

#include "core/domain/values/timeout.hpp"

namespace core::domain {

// Envelope-level: applies to whichever transport the request uses.
struct RequestConfig {
  Timeout timeout;            // config.timeout_ms
  bool tlsEnabledDefault = true; // config.tls

  bool operator==(const RequestConfig &o) const {
    return timeout == o.timeout && tlsEnabledDefault == o.tlsEnabledDefault;
  }
  bool operator!=(const RequestConfig &o) const { return !(*this == o); }
};

} // namespace core::domain
