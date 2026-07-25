// oauth2_token_provider.hpp — ITokenProvider adapter: token-endpoint POST + in-memory token cache.
// INTERNAL (core/src). The pure pieces (parse/cache-key/request building) are free functions here so
// tests cover them without network; nlohmann stays in the .cpp.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "core/domain/auth/auth.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/ports/driven/i_clock.hpp"
#include "core/domain/ports/driven/i_token_provider.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::infra::oauth2 {

// Parsed token-endpoint reply. expiresInSec <= 0 means the server sent no usable expires_in.
struct TokenResponse {
  std::string accessToken;
  long long expiresInSec = 0;
  std::string refreshToken; // empty = none
};

// {"access_token","token_type","expires_in","refresh_token"} -> TokenResponse. Accepts numeric or
// string expires_in. Errors: non-JSON, missing access_token, token_type present and != "bearer"
// (case-insensitive), or an OAuth error body ({"error","error_description"}).
core::domain::Result<TokenResponse> parseTokenResponse(const std::string &body);

// Cache identity of a config (RESOLVED values): tokenUrl|grant|clientId|scope|username.
std::string cacheKey(const core::domain::AuthOAuth2 &o);

// The token-endpoint POST as a domain RequestModel (form-urlencoded; clientAuth==Header -> the model's
// auth is Basic(clientId, clientSecret) and NativeHttpSender does the base64). `refreshToken` non-empty
// -> a grant_type=refresh_token request instead of the configured grant.
core::domain::Result<core::domain::RequestModel>
tokenRequestModel(const core::domain::AuthOAuth2 &o, const core::domain::Timeout &timeout,
                  const std::string &refreshToken = "");

class OAuth2TokenProvider final : public core::domain::ITokenProvider {
public:
  explicit OAuth2TokenProvider(const core::domain::IClock *clock) : clock_(clock) {}

  core::domain::Result<std::string> bearerFor(const core::domain::AuthOAuth2 &oauth,
                                              const core::domain::Timeout &timeout,
                                              const core::domain::ICancellationToken &cancel) override;

private:
  struct Entry {
    std::string token;
    std::chrono::steady_clock::time_point expiry;
    std::string refreshToken;
  };
  // POST `model` and parse the reply (shared by fresh + refresh fetches).
  core::domain::Result<TokenResponse> fetch(const core::domain::RequestModel &model,
                                            const core::domain::ICancellationToken &cancel);

  const core::domain::IClock *clock_; // injected for testable expiry math
  std::mutex mu_;                     // single-flight: concurrent sends -> one fetch, second hits cache
  std::unordered_map<std::string, Entry> cache_;
};

} // namespace core::infra::oauth2
