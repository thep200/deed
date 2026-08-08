#pragma once

#include <string>

#include "core/domain/auth/auth.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/ports/driven/i_cancellation_token.hpp"
#include "core/domain/values/timeout.hpp"

namespace core::domain {

class ITokenProvider {
public:
  virtual ~ITokenProvider() = default;
  // `oauth` must already have {{var}} resolved; blocks at most `timeout`; honors `cancel` cooperatively.
  virtual Result<std::string> bearerFor(const AuthOAuth2 &oauth, const Timeout &timeout,
                                        const ICancellationToken &cancel) = 0;
};

} // namespace core::domain
