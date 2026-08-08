#include <cstdio>
#include <string>

#include "infra/auth/oauth2_token_provider.hpp"

#include "core/domain/body/body.hpp"
#include "core/domain/http/http_request.hpp"

using namespace core;
namespace o2 = core::infra::oauth2;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char *msg) {
  if (ok) ++g_pass;
  else { ++g_fail; std::printf("  FAIL[oauth2_provider]: %s\n", msg); }
}

domain::AuthOAuth2 cc() {
  domain::AuthOAuth2 o;
  o.tokenUrl = "https://idp/token";
  o.clientId = "cid";
  o.clientSecret = "sec";
  o.scope = "read";
  return o;
}

// The token POST's form fields, flattened for assertions.
std::string formOf(const domain::RequestModel &m) {
  std::string out;
  const auto &h = std::get<domain::HttpRequest>(m.payload());
  h.body().match([&](auto &&b) {
    if constexpr (std::is_same_v<std::decay_t<decltype(b)>, domain::BodyFormUrlEncoded>)
      for (const auto &f : b.fields) out += f.key + "=" + f.value + "&";
  });
  return out;
}
} // namespace

int run_oauth2_provider_tests() {
  using domain::ErrorCode;

  {
    auto ok = o2::parseTokenResponse(
        R"({"access_token":"abc","token_type":"Bearer","expires_in":120,"refresh_token":"r1"})");
    check(ok.isOk() && ok.value().accessToken == "abc", "parse: access_token");
    check(ok.isOk() && ok.value().expiresInSec == 120, "parse: numeric expires_in");
    check(ok.isOk() && ok.value().refreshToken == "r1", "parse: refresh_token");
    auto strExp = o2::parseTokenResponse(R"({"access_token":"a","expires_in":"90"})");
    check(strExp.isOk() && strExp.value().expiresInSec == 90, "parse: string expires_in");
    auto noExp = o2::parseTokenResponse(R"({"access_token":"a"})");
    check(noExp.isOk() && noExp.value().expiresInSec == 0, "parse: expires_in absent -> 0 (TTL default later)");
    auto err = o2::parseTokenResponse(R"({"error":"invalid_client","error_description":"bad secret"})");
    check(!err.isOk() && err.error().message == "invalid_client: bad secret", "parse: RFC error body");
    check(!o2::parseTokenResponse(R"({"token_type":"bearer"})").isOk(), "parse: missing access_token");
    auto mac = o2::parseTokenResponse(R"({"access_token":"a","token_type":"MAC"})");
    check(!mac.isOk() && mac.error().code == ErrorCode::Unsupported, "parse: non-bearer token_type");
    check(!o2::parseTokenResponse("<html>").isOk(), "parse: non-JSON body");
  }

  // cacheKey: identity fields only
  {
    auto a = cc(), b = cc();
    check(o2::cacheKey(a) == o2::cacheKey(b), "key: equal configs equal");
    b.scope = "write";
    check(o2::cacheKey(a) != o2::cacheKey(b), "key: scope differs");
    b = cc(); b.clientSecret = "other";
    check(o2::cacheKey(a) == o2::cacheKey(b), "key: secret NOT part of identity");
    b = cc(); b.grant = domain::OAuth2Grant::Password; b.username = "u"; b.password = "p";
    check(o2::cacheKey(a) != o2::cacheKey(b), "key: grant differs");
  }

  // tokenRequestModel: grants + client auth styles
  {
    auto m = o2::tokenRequestModel(cc(), domain::Timeout::fromMillis(1000).take());
    check(m.isOk(), "req: builds");
    std::string form = formOf(m.value());
    check(form.find("grant_type=client_credentials&") != std::string::npos, "req: cc grant_type");
    check(form.find("scope=read&") != std::string::npos, "req: scope included");
    check(form.find("client_id=") == std::string::npos, "req: header style keeps client out of body");
    const auto &h = std::get<domain::HttpRequest>(m.value().payload());
    check(!h.auth().isNone(), "req: header style -> Basic auth on the POST");

    auto body = cc(); body.clientAuth = domain::OAuth2ClientAuth::Body;
    auto mb = o2::tokenRequestModel(body, domain::Timeout::fromMillis(1000).take());
    std::string formB = formOf(mb.value());
    check(formB.find("client_id=cid&") != std::string::npos, "req: body style client_id in form");
    check(formB.find("client_secret=sec&") != std::string::npos, "req: body style secret in form");
    check(std::get<domain::HttpRequest>(mb.value().payload()).auth().isNone(),
          "req: body style -> no Basic header");

    auto pw = cc(); pw.grant = domain::OAuth2Grant::Password; pw.username = "u"; pw.password = "p";
    std::string formP = formOf(o2::tokenRequestModel(pw, domain::Timeout::fromMillis(1000).take()).value());
    check(formP.find("grant_type=password&") != std::string::npos, "req: password grant_type");
    check(formP.find("username=u&") != std::string::npos && formP.find("password=p&") != std::string::npos,
          "req: password credentials in form");

    auto rt = o2::tokenRequestModel(cc(), domain::Timeout::fromMillis(1000).take(), "rtok");
    std::string formR = formOf(rt.value());
    check(formR.find("grant_type=refresh_token&") != std::string::npos, "req: refresh grant_type");
    check(formR.find("refresh_token=rtok&") != std::string::npos, "req: refresh token in form");
    check(formR.find("username=") == std::string::npos, "req: refresh drops password fields");

    auto noUrl = cc(); noUrl.tokenUrl = "";
    check(!o2::tokenRequestModel(noUrl, domain::Timeout::fromMillis(1000).take()).isOk(),
          "req: empty tokenUrl fails");
  }

  std::printf("  oauth2_provider: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
