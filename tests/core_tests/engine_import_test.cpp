#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/app/core_api_client.hpp" // domain stack facade (replaces Engine in these tests)
#include "core/infra/import/importer.hpp"
#include "core/infra/persistence/stores.hpp"
#include "core/infra/serialization/field_json.hpp"

namespace fs = std::filesystem;
using namespace core;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) { ++g_pass; }                                            \
        else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                \
    do {                                                                   \
        auto _va = (a); auto _vb = (b);                                    \
        if (_va == _vb) { ++g_pass; }                                      \
        else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

static std::string makeTempRoot() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = fs::temp_directory_path() / ("apiclient_test_" + std::to_string(stamp));
    fs::remove_all(base);
    fs::create_directories(base);
    return base.string();
}

namespace { // internal linkage: persistence_store_test.cpp carries its own ts copy
namespace ts {
namespace d = core::domain;
const d::HttpRequest& http(const d::RequestModel& m) { return std::get<d::HttpRequest>(m.payload()); }
const d::WebSocketRequest& ws(const d::RequestModel& m) { return std::get<d::WebSocketRequest>(m.payload()); }
const d::GraphQlRequest& gql(const d::RequestModel& m) { return std::get<d::GraphQlRequest>(m.payload()); }
std::string bearerOf(const d::Auth& a) {
    std::string t;
    a.match([&](auto&& x) { using T = std::decay_t<decltype(x)>; if constexpr (std::is_same_v<T, d::AuthBearer>) t = x.token; });
    return t;
}
} // namespace ts
} // namespace

static void test_engine(const std::string& root) {
    std::printf("[engine]\n");
    // prepare env Shared + active ("Global" reserved — see test_global_env).
    EnvironmentStore env(root);
    Environment g; g.name = "Shared"; g.keys.push_back({"baseUrl", "http://global", true});
    g.keys.push_back({"wsBase", "ws://global", true}); // ws-scheme prefix (domain ws urls must be ws/wss)
    env.save(g);
    Environment d; d.name = "Stage"; d.keys.push_back({"baseUrl", "http://stage", true});
    env.save(d);

    auto client = core::app::CoreApiClient::create(
        core::app::CoreApiClient::Config{root, (fs::path(root) / "appconfig.json").string()});
    client->session().setActiveEnv("Stage");

    CHECK_EQ(client->resolvePreview("{{baseUrl}}/x"), std::string("http://stage/x"), "active env Stage");
    client->session().setActiveEnv("Shared");
    CHECK_EQ(client->resolvePreview("{{baseUrl}}/x"), std::string("http://global/x"), "active env Shared");
    CHECK_EQ(client->resolvePreview("{{missing}}"), std::string("{{missing}}"), "missing var keeps literal");

    CHECK(client->validateJson(core::domain::JsonText::of("{\"a\": 1}")).isOk(), "valid JSON");
    CHECK(!client->validateJson(core::domain::JsonText::of("{\"a\": }")).isOk(), "invalid JSON caught");

    namespace d2 = core::domain;
    const d2::RequestConfig cfg{d2::Timeout::fromMillis(1800000).take(), true};
    auto hdr = [](const std::string &k, const std::string &v) { return d2::Header::create(k, v).take(); };
    auto mkHttp = [&](const std::string &url, std::vector<d2::Header> hdrs) {
      d2::HttpRequest::Parts hp{d2::HttpMethod::Get, d2::Url::create(url).take(), d2::PathVariableList{},
                                d2::QueryParamList{}, d2::HeaderList{std::move(hdrs)}, d2::Body::none(),
                                d2::Auth::none()};
      return d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                      d2::HttpRequest::create(std::move(hp)).take())
          .take();
    };
    auto aliasify = [&](const d2::RequestModel &mm) { return client->aliasifyModel(mm); };

    std::string curl = client->exportCurl(mkHttp("{{baseUrl}}/u", {}));
    CHECK(curl.find("http://global/u") != std::string::npos, "exportCurl resolves url");

    // --- aliasifyModel: literal values matching the env are rewritten back to {{alias}} ---
    // env "baseUrl" = http://global (active env is Shared at this point).
    d2::RequestModel ax =
        aliasify(mkHttp("http://global/users", {hdr("Host", "http://global"), hdr("X-Lit", "literal")}));
    CHECK_EQ(ts::http(ax).url().raw(), std::string("{{baseUrl}}/users"), "url prefix aliasified");
    CHECK_EQ(ts::http(ax).headers().items()[0].value(), std::string("{{baseUrl}}"), "header whole aliasified");
    CHECK_EQ(ts::http(ax).headers().items()[1].value(), std::string("literal"), "non-match left unchanged");

    d2::RequestModel ax2 = aliasify(ax);
    CHECK_EQ(ts::http(ax2).url().raw(), std::string("{{baseUrl}}/users"), "url stable on second pass");

    // aliasify also covers WebSocket + GraphQL (import alias-replace) — env baseUrl=http://global active.
    d2::WebSocketRequest::Parts wqp{d2::Url::create("ws://global/socket").take()}; // ws url must be ws/wss
    wqp.auth = d2::Auth::bearer("http://global").take();                            // whole-value match
    d2::RequestModel wqx = aliasify(
        d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                 d2::WebSocketRequest::create(std::move(wqp)).take())
            .take());
    CHECK_EQ(ts::ws(wqx).url().raw(), std::string("{{wsBase}}/socket"), "ws url aliasified");
    CHECK_EQ(ts::bearerOf(ts::ws(wqx).auth()), std::string("{{baseUrl}}"), "ws auth aliasified");

    d2::GraphQlOperation gop;
    gop.query = "query { me }"; // domain graphql needs a query
    d2::GraphQlRequest::Parts gqp{d2::Url::create("http://global/graphql").take(), gop,
                                  d2::HeaderList{std::vector<d2::Header>{hdr("X-Base", "http://global")}},
                                  d2::Auth::none(), d2::GqlSubTransport::Http, ""};
    d2::RequestModel gqx = aliasify(
        d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                 d2::GraphQlRequest::create(std::move(gqp)).take())
            .take());
    CHECK_EQ(ts::gql(gqx).url().raw(), std::string("{{baseUrl}}/graphql"), "graphql url aliasified");
    CHECK_EQ(ts::gql(gqx).headers().items()[0].value(), std::string("{{baseUrl}}"), "graphql header aliasified");

    // duplicate values -> first-defined key wins ("zdup" before "adup").
    Environment go; go.name = "Shared";
    go.keys.push_back({"zdup", "http://dup.host", true});
    go.keys.push_back({"adup", "http://dup.host", true});
    client->environments().save(go);
    client->session().setActiveEnv("Shared");
    d2::RequestModel dupOut = aliasify(mkHttp("http://dup.host/p", {}));
    CHECK_EQ(ts::http(dupOut).url().raw(), std::string("{{zdup}}/p"), "duplicate value -> first-defined key wins");

    // --- interactionOf: routing by gRPC method type ---
    auto mkGrpc = [&](d2::GrpcMethodType mt) {
      d2::GrpcRequest::Parts gp;
      gp.methodType = mt;
      return d2::RequestModel::create(d2::RequestId(""), "t", 0, cfg,
                                      d2::GrpcRequest::create(std::move(gp)).take())
          .take();
    };
    auto io = [&](const d2::RequestModel &mm) { return client->interactionOf(mm); };
    CHECK(io(mkGrpc(d2::GrpcMethodType::Unary)) == InteractionKind::Unary, "unary -> Unary");
    CHECK(io(mkGrpc(d2::GrpcMethodType::ServerStreaming)) == InteractionKind::ServerStream,
          "server_streaming -> ServerStream");
    CHECK(io(mkGrpc(d2::GrpcMethodType::ClientStreaming)) == InteractionKind::ClientStream,
          "client_streaming -> ClientStream");
    CHECK(io(mkGrpc(d2::GrpcMethodType::BidiStreaming)) == InteractionKind::BiDi, "bidi_streaming -> BiDi");
    CHECK(io(mkHttp("http://x", {})) == InteractionKind::Unary, "http -> Unary");
}

namespace impv { // small views over the domain payload so the importer checks stay readable
namespace d = core::domain;
const d::HttpRequest& httpOf(const core::ImportParseResult& r) {
    return std::get<d::HttpRequest>(r.model->payload());
}
const d::GrpcRequest& grpcOf(const core::ImportParseResult& r) {
    return std::get<d::GrpcRequest>(r.model->payload());
}
struct BodyView { std::string mode, content; };
BodyView bodyView(const d::Body& b) {
    BodyView v{"none", ""};
    b.match([&](auto&& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, d::BodyRaw>) {
            v.mode = x.subtype == d::RawSubtype::Json ? "json" : x.subtype == d::RawSubtype::Xml ? "xml" : "text";
            v.content = x.text;
        } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) v.mode = "form-urlencoded";
        else if constexpr (std::is_same_v<T, d::BodyMultipart>) v.mode = "multipart";
        else if constexpr (std::is_same_v<T, d::BodyBinary>) v.mode = "binary";
    });
    return v;
}
struct AuthView { std::string type, bearer, basicUser; };
AuthView authView(const d::Auth& a) {
    AuthView v{"none", "", ""};
    a.match([&](auto&& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, d::AuthBearer>) { v.type = "bearer"; v.bearer = x.token; }
        else if constexpr (std::is_same_v<T, d::AuthBasic>) { v.type = "basic"; v.basicUser = x.username; }
    });
    return v;
}
} // namespace impv

static void test_importers() {
    std::printf("[importers]\n");
    CurlImporter curl;
    CHECK(curl.canHandle("curl http://x"), "canHandle curl");
    CHECK(!curl.canHandle("wget http://x"), "does not accept wget");

    auto r = curl.parse("curl -X POST 'http://api.test/users?q=1' "
                        "-H 'Content-Type: application/json' "
                        "-H 'Authorization: Bearer abc' "
                        "-d '{\"name\":\"Alice\"}'");
    CHECK(r.ok && r.model, "parse curl ok");
    const auto& h = impv::httpOf(r);
    CHECK_EQ(core::domain::toString(h.method()), std::string("POST"), "method POST");
    CHECK_EQ(h.url().raw(), std::string("http://api.test/users"), "url with query stripped");
    CHECK_EQ(h.params().size(), size_t(1), "query split into 1 param");
    CHECK_EQ(h.params().items()[0].key(), std::string("q"), "param key q");
    CHECK_EQ(h.params().items()[0].value(), std::string("1"), "param value 1");
    CHECK_EQ(impv::bodyView(h.body()).mode, std::string("json"), "body json from content-type");
    CHECK_EQ(impv::bodyView(h.body()).content, std::string("{\"name\":\"Alice\"}"), "body content");
    CHECK_EQ(h.headers().size(), size_t(1), "1 header (Authorization -> Auth)");
    CHECK_EQ(impv::authView(h.auth()).type, std::string("bearer"), "Authorization Bearer -> bearer auth");
    CHECK_EQ(impv::authView(h.auth()).bearer, std::string("abc"), "bearer token into Auth tab");

    auto rb = curl.parse("curl -u user:pass http://api.test/secure");
    CHECK_EQ(impv::authView(impv::httpOf(rb).auth()).type, std::string("basic"), "-u -> basic auth");
    CHECK_EQ(impv::authView(impv::httpOf(rb).auth()).basicUser, std::string("user"), "basic user");
    CHECK_EQ(core::domain::toString(impv::httpOf(rb).method()), std::string("GET"), "no body -> GET");

    GrpcImporter g;
    CHECK(g.canHandle("grpcurl -plaintext localhost:50051 pkg.Svc/M"), "canHandle grpcurl");
    auto gr = g.parse("grpcurl -plaintext -d '{\"id\":\"1\"}' -H 'authorization: Bearer t' "
                      "localhost:50051 user.v1.UserService/GetUser");
    CHECK(gr.ok && gr.model, "parse grpcurl ok");
    CHECK_EQ(impv::grpcOf(gr).target(), std::string("localhost:50051"), "target");
    // Import intentionally SKIPS the RPC (Service/Method) — only target/message/metadata/tls are imported.
    CHECK(impv::grpcOf(gr).service().empty(), "service skipped on import");
    CHECK(impv::grpcOf(gr).method().empty(), "method skipped on import");
    CHECK_EQ(impv::grpcOf(gr).tls().enabled(), false, "-plaintext -> tls off");
    CHECK_EQ(impv::grpcOf(gr).metadata().entries().size(), size_t(1), "1 metadata");

    auto gr2 = g.parse("grpcs://localhost:50051/pkg.Service/Method");
    CHECK(gr2.ok && gr2.model, "parse compact string ok");
    CHECK_EQ(impv::grpcOf(gr2).tls().enabled(), true, "grpcs -> tls on");
    CHECK_EQ(impv::grpcOf(gr2).target(), std::string("localhost:50051"), "target from compact string");
    CHECK(impv::grpcOf(gr2).service().empty(), "compact: service skipped on import");
}

static void test_audit_fixes() {
    std::printf("[audit_fixes]\n");

    // pathologically deep JSON is rejected by the depth guard (returns false, does NOT crash).
    {
        std::string deep(500, '[');   // 500 levels, well past kMaxJsonDepth
        CHECK(!core::serial::jsonToHeaders(deep).isOk(), "H5: deep JSON rejected, no stack overflow");
        CHECK(core::serial::jsonToHeaders("[]").isOk(), "H5: shallow JSON still parses");
    }

    CurlImporter curl;
    // valid Basic decodes; malformed Basic is NOT silently accepted as credentials.
    {
        auto good = curl.parse("curl -H 'Authorization: Basic dXNlcjpwYXNz' http://x.test");  // user:pass
        auto gv = impv::authView(impv::httpOf(good).auth());
        CHECK(good.ok && gv.type == "basic" && gv.basicUser == "user", "M15: valid Basic -> basic creds");
        auto bad = curl.parse("curl -H 'Authorization: Basic not_base64!!' http://x.test");
        const auto& badH = impv::httpOf(bad);
        CHECK(bad.ok && impv::authView(badH.auth()).type == "none" && badH.headers().size() == 1 &&
                  badH.headers().items()[0].name() == "Authorization",
              "M15: malformed Basic -> raw Authorization header kept (not garbled creds)");
    }
    // an empty inline value (--data=) must not swallow the next token as data.
    {
        auto dd = curl.parse("curl --data= http://x.test");
        CHECK(dd.ok && impv::httpOf(dd).url().raw().find("x.test") != std::string::npos,
              "M14: empty --data= keeps the URL");
    }

    // Per-request config: grpc import carries TLS intent into RequestConfig.tls.
    {
        GrpcImporter g;
        auto plain = g.parse("grpcurl -plaintext localhost:50051 pkg.Svc/M");
        CHECK(plain.ok && plain.model->config().tlsEnabledDefault == false, "config.tls follows -plaintext (off)");
        auto secure = g.parse("grpcs://localhost:50051/pkg.Svc/M");
        CHECK(secure.ok && secure.model->config().tlsEnabledDefault == true, "config.tls follows grpcs:// (on)");
    }
}

int run_engine_import_tests() {
    std::string root = makeTempRoot();

    test_engine(root);
    test_importers();
    test_audit_fixes();

    fs::remove_all(root);
    std::printf("  engine_import: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
