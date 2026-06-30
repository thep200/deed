// domain_tests.cpp — REFACTOR_SPEC §11.1 domain unit tests. Header-only domain; this TU links NO core lib.
// Each factory: a valid case + each invariant violation returns the right ErrorCode/field; sum types
// `match` every alternative; value objects compare by value.
#include <cstdio>
#include <string>
#include <vector>

#include "core/domain/auth/auth.hpp"
#include "core/domain/body/body.hpp"
#include "core/domain/common/enabled_flag.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/common/strong_string.hpp"
#include "core/domain/graphql/graphql_request.hpp"
#include "core/domain/grpc/grpc_metadata.hpp"
#include "core/domain/grpc/grpc_request.hpp"
#include "core/domain/http/http_request.hpp"
#include "core/domain/values/header.hpp"
#include "core/domain/values/http_method.hpp"
#include "core/domain/values/json_text.hpp"
#include "core/domain/values/path_variable.hpp"
#include "core/domain/values/query_param.hpp"
#include "core/domain/values/timeout.hpp"
#include "core/domain/values/tls_config.hpp"
#include "core/domain/values/url.hpp"
#include "core/domain/ws/websocket_request.hpp"

using namespace core::domain;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                                           \
  do {                                                                                             \
    if (cond) { ++g_pass; }                                                                        \
    else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }              \
  } while (0)

static void test_result_and_common() {
  std::printf("[result/common]\n");
  auto okR = Result<int>::ok(42);
  CHECK(okR.isOk() && okR.value() == 42, "Result ok carries value");
  auto badR = Result<int>::fail({ErrorCode::Validation, "nope", "f"});
  CHECK(!badR.isOk() && badR.error().code == ErrorCode::Validation, "Result fail carries error");
  CHECK(badR.valueOr(7) == 7, "valueOr returns fallback on error");
  CHECK(ok().isOk(), "Status ok()");
  CHECK(!fail({ErrorCode::Internal, "x"}).isOk(), "Status fail()");

  using Rid = StrongString<struct RidTag>;
  Rid a{"req_1"}, b{"req_1"}, c{"req_2"};
  CHECK(a == b && a != c, "StrongString value equality");
  CHECK(std::hash<Rid>{}(a) == std::hash<Rid>{}(b), "StrongString hashable & stable");

  CHECK(EnabledFlag::fromInt(0).value() == false, "EnabledFlag 0 -> false");
  CHECK(EnabledFlag::fromInt(1).toInt() == 1, "EnabledFlag round-trip 1");
}

static void test_values() {
  std::printf("[values]\n");
  CHECK(Url::create("   ").value().empty(), "Url allows blank (draft) -> empty");
  auto u = Url::create("  https://api/x  ");
  CHECK(u.isOk() && u.value().raw() == "https://api/x", "Url trims & keeps");
  CHECK(u.value().scheme() == "https", "Url scheme extracted lowercased");
  CHECK(Url::create("{{base}}/x").value().startsWithPlaceholder(), "Url placeholder detected");
  CHECK(!Url::createWithSchemes("http://x", {"ws", "wss"}).isOk(), "Url scheme set rejects http for ws");
  CHECK(Url::createWithSchemes("{{base}}", {"ws", "wss"}).isOk(), "Url placeholder bypasses scheme check");

  CHECK(parseHttpMethod("post").value() == HttpMethod::Post, "method parse case-insensitive");
  CHECK(toString(HttpMethod::Delete) == "DELETE", "method toString");
  CHECK(!parseHttpMethod("FETCH").isOk(), "unknown method rejected");

  CHECK(!Timeout::fromMillis(0).isOk(), "timeout 0 rejected");
  CHECK(Timeout::fromMillis(1500).value().millis() == 1500, "timeout value");

  CHECK(Header::create("", "v", true).error().field == "header.name", "enabled header needs name");
  CHECK(Header::create("", "v", false).isOk(), "disabled header may be nameless");
  CHECK(!Header::create("bad name", "v").isOk(), "header name token validated");
  auto h = Header::create("Content-Type", "application/json").value();
  CHECK(h.name() == "Content-Type" && h.enabled(), "header fields");

  HeaderList hl(std::vector<Header>{h, Header::create("X-Off", "1", false).value()});
  CHECK(hl.enabledOnly().size() == 1, "HeaderList enabledOnly");
  CHECK(hl.find("content-type").has_value(), "HeaderList case-insensitive find");

  CHECK(!QueryParam::create("", "v").isOk(), "enabled query param needs key");
  CHECK(!PathVariable::create("a b", "v").isOk(), "path var key validated");
  CHECK(JsonText::emptyObject().text() == "{}", "JsonText empty object");

  auto tls = TlsConfig::create(true, false, "", "cert.pem", "");
  CHECK(tls.hasClientCertWithoutKey(), "tls soft warning surfaced");
}

static void test_auth() {
  std::printf("[auth]\n");
  CHECK(Auth::none().isNone(), "auth none");
  CHECK(!Auth::basic("", "p").isOk(), "basic needs username");
  CHECK(!Auth::bearer("").isOk(), "bearer needs token");
  CHECK(!Auth::apiKey("", "v", ApiKeyIn::Header).isOk(), "apikey needs name");
  auto bearer = Auth::bearer("tok").value();
  CHECK(bearer == Auth::bearer("tok").value(), "auth value equality");
  CHECK(bearer != Auth::none(), "auth inequality across alternatives");

  // match handles every alternative.
  std::string kind = bearer.match([](auto &&a) -> std::string {
    using T = std::decay_t<decltype(a)>;
    if constexpr (std::is_same_v<T, AuthNone>) return "none";
    else if constexpr (std::is_same_v<T, AuthBasic>) return "basic";
    else if constexpr (std::is_same_v<T, AuthBearer>) return "bearer";
    else return "apikey";
  });
  CHECK(kind == "bearer", "auth match dispatches correct alternative");
}

static void test_body() {
  std::printf("[body]\n");
  CHECK(Body::none().isNone(), "body none");
  CHECK(Body::raw(RawSubtype::Json, "{}") == Body::raw(RawSubtype::Json, "{}"), "body raw equality");
  CHECK(Body::raw(RawSubtype::Json, "{}") != Body::raw(RawSubtype::Text, "{}"), "body subtype matters");
  CHECK(!Body::binary("").isOk(), "binary body needs path");
  CHECK(Body::binary("/tmp/x").isOk(), "binary body ok");
  std::vector<MultipartPart> parts{{"f", PartKind::File, "", "", true}};
  CHECK(!Body::multipart(parts).isOk(), "multipart file part needs filePath");
  auto form = Body::formUrlEncoded({{"k", "v", true}});
  std::string tag = form.match([](auto &&b) -> std::string {
    using T = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<T, BodyFormUrlEncoded>) return "form";
    else return "other";
  });
  CHECK(tag == "form", "body match dispatches form");
}

static void test_grpc_graphql_ws_http() {
  std::printf("[requests]\n");
  CHECK(!GrpcMetadata::create({{"Bad Key", "v", true}}).isOk(), "grpc metadata key validated");
  CHECK(GrpcMetadata::create({{"x-trace-bin", "v", true}}).isOk(), "grpc -bin key allowed");
  CHECK(GrpcRequest::create({}).isOk(), "grpc allows empty target (draft; enforced at send)");
  GrpcRequest::Parts gp;
  gp.target = "localhost:50051";
  CHECK(GrpcRequest::create(gp).isOk(), "grpc with target ok");

  CHECK(!ProtoSource::files({}, {}).isOk(), "protoFiles needs a file");
  CHECK(ProtoSource::reflection().match([](auto &&s) {
    return std::is_same_v<std::decay_t<decltype(s)>, ProtoReflection>;
  }), "proto source match reflection");

  GraphQlRequest::Parts gq{Url::create("https://api/graphql").take(), {}, {}, Auth::none(),
                           GqlSubTransport::Http, ""};
  gq.op.query = "{ me { id } }";
  CHECK(GraphQlRequest::create(gq).isOk(), "graphql query ok");
  GraphQlRequest::Parts gqSub = gq;
  gqSub.op.operation = GqlOperationType::Subscription; // still Http -> must fail
  CHECK(!GraphQlRequest::create(gqSub).isOk(), "graphql subscription requires ws");

  CHECK(!WebSocketRequest::create({Url::create("http://x").take()}).isOk(), "ws rejects http scheme");
  CHECK(WebSocketRequest::create({Url::create("wss://x").take()}).isOk(), "wss ok");

  HttpRequest::Parts hp{HttpMethod::Post, Url::create("https://api/users").take()};
  hp.headers = HeaderList(std::vector<Header>{Header::create("Accept", "*/*").value()});
  hp.body = Body::raw(RawSubtype::Json, "{\"a\":1}");
  auto http = HttpRequest::create(std::move(hp));
  CHECK(http.isOk() && http.value().method() == HttpMethod::Post, "http request assembled");
}

int main() {
  std::printf("== domain_tests ==\n");
  test_result_and_common();
  test_values();
  test_auth();
  test_body();
  test_grpc_graphql_ws_http();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
