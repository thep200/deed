#include "infra/auth/oauth2_token_provider.hpp"

#include <cctype>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/domain/body/body.hpp"
#include "core/domain/http/http_request.hpp"
#include "core/domain/ports/driven/i_response_sink.hpp"
#include "core/domain/response/response_event.hpp"
#include "infra/transport/http/native_http_sender.hpp"

namespace core::infra::oauth2 {
namespace d = core::domain;
using nlohmann::json;

namespace {

constexpr long long kDefaultTtlSec = 3600; // expires_in absent -> assume 1h (re-fetch is cheap anyway)
constexpr long long kExpirySkewSec = 30;   // treat tokens this close to expiry as already expired

template <class T> d::Result<T> fail(d::ErrorCode c, const std::string &msg) {
  return d::Result<T>::fail({c, msg, ""});
}

bool iequals(const std::string &a, const char *b) {
  std::size_t n = 0;
  for (; b[n] && n < a.size(); ++n)
    if (std::tolower((unsigned char)a[n]) != std::tolower((unsigned char)b[n])) return false;
  return n == a.size() && b[n] == '\0';
}

const char *grantValue(d::OAuth2Grant g) {
  return g == d::OAuth2Grant::Password ? "password" : "client_credentials";
}

} // namespace

d::Result<TokenResponse> parseTokenResponse(const std::string &body) {
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception &e) {
    return fail<TokenResponse>(d::ErrorCode::Parse,
                               std::string("token response is not JSON: ") + e.what());
  }
  if (!j.is_object()) return fail<TokenResponse>(d::ErrorCode::Parse, "token response is not an object");
  if (j.contains("error")) { // RFC 6749 §5.2 error body
    std::string msg = j.value("error", std::string());
    std::string desc = j.value("error_description", std::string());
    return fail<TokenResponse>(d::ErrorCode::Network, desc.empty() ? msg : msg + ": " + desc);
  }
  TokenResponse out;
  out.accessToken = j.value("access_token", std::string());
  if (out.accessToken.empty())
    return fail<TokenResponse>(d::ErrorCode::Parse, "token response has no access_token");
  std::string type = j.value("token_type", std::string());
  if (!type.empty() && !iequals(type, "bearer"))
    return fail<TokenResponse>(d::ErrorCode::Unsupported, "unsupported token_type: " + type);
  if (auto it = j.find("expires_in"); it != j.end()) {
    if (it->is_number()) out.expiresInSec = it->get<long long>();
    else if (it->is_string()) {
      try { out.expiresInSec = std::stoll(it->get<std::string>()); } catch (...) {}
    }
  }
  out.refreshToken = j.value("refresh_token", std::string());
  return d::Result<TokenResponse>::ok(std::move(out));
}

std::string cacheKey(const d::AuthOAuth2 &o) {
  return o.tokenUrl + "|" + grantValue(o.grant) + "|" + o.clientId + "|" + o.scope + "|" + o.username;
}

d::Result<d::RequestModel> tokenRequestModel(const d::AuthOAuth2 &o, const d::Timeout &timeout,
                                             const std::string &refreshToken) {
  auto url = d::Url::create(o.tokenUrl);
  if (!url.isOk() || url.value().raw().empty())
    return fail<d::RequestModel>(d::ErrorCode::Validation, "oauth2 tokenUrl required");

  std::vector<d::FormField> form;
  if (!refreshToken.empty()) {
    form.push_back({"grant_type", "refresh_token", true});
    form.push_back({"refresh_token", refreshToken, true});
  } else {
    form.push_back({"grant_type", grantValue(o.grant), true});
    if (o.grant == d::OAuth2Grant::Password) {
      form.push_back({"username", o.username, true});
      form.push_back({"password", o.password, true});
    }
  }
  if (!o.scope.empty()) form.push_back({"scope", o.scope, true});

  // Client authentication: Basic header (sender encodes) or explicit body params (some IdPs insist).
  d::Auth auth = d::Auth::none();
  if (o.clientAuth == d::OAuth2ClientAuth::Header) {
    auto b = d::Auth::basic(o.clientId, o.clientSecret);
    if (!b.isOk()) return fail<d::RequestModel>(b.error().code, b.error().message);
    auth = b.take();
  } else {
    form.push_back({"client_id", o.clientId, true});
    if (!o.clientSecret.empty()) form.push_back({"client_secret", o.clientSecret, true});
  }

  d::HttpRequest::Parts hp{d::HttpMethod::Post,
                           url.take(),
                           {},
                           {},
                           {},
                           d::Body::formUrlEncoded(std::move(form)),
                           std::move(auth)};
  auto http = d::HttpRequest::create(std::move(hp));
  if (!http.isOk()) return fail<d::RequestModel>(http.error().code, http.error().message);
  d::RequestConfig cfg{timeout, true};
  return d::RequestModel::create(d::RequestId("oauth2_token"), "oauth2 token", 0, cfg, http.take());
}

d::Result<TokenResponse> OAuth2TokenProvider::fetch(const d::RequestModel &model,
                                                    const d::ICancellationToken &cancel) {
  struct CaptureSink final : d::IResponseSink {
    std::optional<d::ApiResponse> ok;
    std::optional<d::ApiError> err;
    void emit(const d::ResponseEvent &ev) override {
      if (const auto *c = ev.get<d::EvCompleted>()) ok = c->summary;
      else if (const auto *f = ev.get<d::EvFailed>()) err = f->error;
    }
  } sink;

  infra::NativeHttpSender http;
  d::Status st = http.execute(model, sink, cancel);
  if (sink.err) return fail<TokenResponse>(d::ErrorCode::Network, sink.err->message);
  if (!sink.ok)
    return fail<TokenResponse>(d::ErrorCode::Network,
                               st ? std::string("no token response") : st.error().message);
  // The RFC error body is more precise than a bare status code — parse it even for HTTP >= 400.
  auto parsed = parseTokenResponse(sink.ok->body);
  if (!parsed.isOk() && sink.ok->statusCode >= 400 && parsed.error().code == d::ErrorCode::Parse)
    return fail<TokenResponse>(d::ErrorCode::Network,
                               "HTTP " + std::to_string(sink.ok->statusCode) + ": " +
                                   sink.ok->body.substr(0, 200));
  return parsed;
}

d::Result<std::string> OAuth2TokenProvider::bearerFor(const d::AuthOAuth2 &oauth,
                                                      const d::Timeout &timeout,
                                                      const d::ICancellationToken &cancel) {
  std::lock_guard<std::mutex> lk(mu_); // single-flight: 2nd concurrent send waits, then hits the cache
  const std::string key = cacheKey(oauth);
  const auto now = clock_ ? clock_->now() : std::chrono::steady_clock::now();

  auto it = cache_.find(key);
  if (it != cache_.end() && now < it->second.expiry) return d::Result<std::string>::ok(it->second.token);

  // Refresh grant first when we hold a refresh token, else configured grant; both failing -> full-grant error wins.
  std::string refreshToken = it != cache_.end() ? it->second.refreshToken : std::string();
  d::Result<TokenResponse> res = fail<TokenResponse>(d::ErrorCode::Internal, "unreachable");
  bool haveRes = false;
  if (!refreshToken.empty()) {
    auto rm = tokenRequestModel(oauth, timeout, refreshToken);
    if (rm.isOk()) {
      res = fetch(rm.take(), cancel);
      haveRes = res.isOk();
    }
  }
  if (!haveRes) {
    auto m = tokenRequestModel(oauth, timeout);
    if (!m.isOk()) return fail<std::string>(m.error().code, m.error().message);
    res = fetch(m.take(), cancel);
  }
  if (!res.isOk()) {
    cache_.erase(key); // a stale entry must not shadow the failure on the next call
    return d::Result<std::string>::fail(res.error());
  }

  TokenResponse tok = res.take();
  long long ttl = tok.expiresInSec > 0 ? tok.expiresInSec : kDefaultTtlSec;
  Entry e;
  e.token = tok.accessToken;
  e.expiry = now + std::chrono::seconds(ttl > kExpirySkewSec ? ttl - kExpirySkewSec : ttl);
  e.refreshToken = tok.refreshToken;
  cache_[key] = e;
  return d::Result<std::string>::ok(std::move(tok.accessToken));
}

} // namespace core::infra::oauth2
