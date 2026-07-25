// core/domain/ports/driven/i_token_provider.hpp — OAuth2 access-token acquisition (REFACTOR_SPEC §6.3).
// Returns the bearer string only; caching/expiry/refresh are the adapter's concern, so this port stays
// stable when new grants (auth-code/PKCE) arrive.
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
  // `oauth` must already have {{var}} resolved. Blocks the calling thread for at most `timeout`
  // (the request's per-request config timeout); honors `cancel` cooperatively.
  virtual Result<std::string> bearerFor(const AuthOAuth2 &oauth, const Timeout &timeout,
                                        const ICancellationToken &cancel) = 0;
};

} // namespace core::domain
