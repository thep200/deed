#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core/infra/export/exporter.hpp" // toCurl (export)
#include "core/infra/persistence/request_naming.hpp"
#include "core/infra/variables/variable_resolver.hpp"

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

static void test_variable_resolver() {
    std::printf("[variable_resolver]\n");
    std::map<std::string, std::string> vars{{"baseUrl", "http://x"}, {"empty", ""}};

    auto r1 = VariableResolver::resolve("{{baseUrl}}/users", vars);
    CHECK_EQ(r1.text, std::string("http://x/users"), "basic resolve");
    CHECK(r1.missing.empty(), "no missing");

    auto r2 = VariableResolver::resolve("a{{empty}}b", vars);
    CHECK_EQ(r2.text, std::string("ab"), "empty var -> \"\"");

    auto r3 = VariableResolver::resolve("x{{nope}}y", vars);
    CHECK_EQ(r3.text, std::string("x{{nope}}y"), "missing var -> keep literal");
    CHECK_EQ(r3.missing.size(), size_t(1), "record 1 missing");

    auto r4 = VariableResolver::resolve("{{ baseUrl }}", vars);
    CHECK_EQ(r4.text, std::string("http://x"), "trim whitespace inside {{ }}");
}

static void test_alias_inversion() {
    std::printf("[alias_inversion]\n");
    std::vector<std::pair<std::string, std::string>> vars{
        {"baseUrl", "https://api.example.com"},
        {"token", "secretXYZ"},
        {"empty", ""},
        {"zfirst", "shared"},
        {"afirst", "shared"},
    };
    std::string out, key;

    CHECK(VariableResolver::valueToAlias("secretXYZ", vars, out, &key), "whole match found");
    CHECK_EQ(out, std::string("{{token}}"), "whole -> {{token}}");
    CHECK_EQ(key, std::string("token"), "whole reports key");

    CHECK(!VariableResolver::valueToAlias("not-in-env", vars, out), "whole no match");
    // empty env value never matches an empty field (avoids aliasing everything)
    CHECK(!VariableResolver::valueToAlias("", vars, out), "empty field no match");

    // duplicate values -> the FIRST-defined key wins (zfirst before afirst), NOT smallest key.
    CHECK(VariableResolver::valueToAlias("shared", vars, out, &key), "tie match");
    CHECK_EQ(key, std::string("zfirst"), "tie -> first-defined key");

    CHECK(VariableResolver::prefixToAlias("https://api.example.com/v1/users", vars, out, &key),
          "prefix match found");
    CHECK_EQ(out, std::string("{{baseUrl}}/v1/users"), "prefix -> {{baseUrl}}/rest");

    CHECK(VariableResolver::prefixToAlias("https://api.example.com", vars, out),
          "prefix exact match");
    CHECK_EQ(out, std::string("{{baseUrl}}"), "prefix exact -> {{baseUrl}}");

    // duplicate prefix values of equal length -> first-defined wins (host1 before host2).
    std::vector<std::pair<std::string, std::string>> hosts{
        {"host1", "http://dup.local"}, {"host2", "http://dup.local"}};
    CHECK(VariableResolver::prefixToAlias("http://dup.local/x", hosts, out, &key), "dup prefix match");
    CHECK_EQ(key, std::string("host1"), "dup prefix -> first-defined key");

    CHECK(!VariableResolver::prefixToAlias("{{baseUrl}}/v1", vars, out), "no re-alias of {{ }}");

    // short value below the prefix floor must NOT mangle text
    std::vector<std::pair<std::string, std::string>> shortVars{{"x", "ht"}};
    CHECK(!VariableResolver::prefixToAlias("http://h", shortVars, out), "short prefix ignored");
}

static void test_request_naming() {
    std::printf("[request_naming]\n");

    // grammar: <id>_<type>_...; first token = id; slug keeps '_'/'-'.
    auto g = parseRequestFilename("ab12cd_grpc_get-list-user.json");
    CHECK(g.ok && g.type == RequestType::Grpc, "grpc parse ok");
    CHECK_EQ(g.id, std::string("ab12cd"), "id = first token");
    CHECK_EQ(g.slug, std::string("get-list-user"), "grpc slug = part after grpc_");
    CHECK(g.method.empty(), "grpc has NO method");

    auto h = parseRequestFilename("xy9z_http_get_get_list_user.json");
    CHECK(h.ok && h.type == RequestType::Http, "http parse ok");
    CHECK_EQ(h.id, std::string("xy9z"), "id = first token");
    CHECK_EQ(h.method, std::string("get"), "http method");
    CHECK_EQ(h.slug, std::string("get_list_user"), "http slug keeps '_'");

    // No back-compat: a name without an id is not our grammar -> rejected.
    CHECK(!parseRequestFilename("http_get_tours-configs.json").ok, "id-less name -> ok=false");

    CHECK(!parseRequestFilename("collection.json").ok, "name not matching grammar -> ok=false");
    CHECK(!parseRequestFilename("README.md").ok, "no '_' -> ok=false");
    CHECK(!parseRequestFilename("ab12_xxx_slug.json").ok, "unknown type -> ok=false");

    CHECK(isValidFileId("ab12cd34"), "valid base36 id");
    CHECK(!isValidFileId("req_ABC"), "legacy id with '_'/uppercase -> invalid");
    CHECK(!isValidFileId(""), "empty id -> invalid");

    CHECK_EQ(normalizeDisplayName("get-list-user"), std::string("Get list user"), "de-slug grpc");
    CHECK_EQ(normalizeDisplayName("get_list_user"), std::string("Get list user"), "de-slug '_'");
    CHECK_EQ(normalizeDisplayName("name@of#request!"), std::string("Nameofrequest"),
             "special chars dropped");

    CHECK_EQ(encodeRequestFilename("k7id", RequestType::Http, "POST", "Create Tour"),
             std::string("k7id_http_post_create-tour.json"), "encode http: id + method");
    CHECK_EQ(encodeRequestFilename("k7id", RequestType::Grpc, "", "Get List User"),
             std::string("k7id_grpc_get-list-user.json"), "encode grpc: id, NO method");

    std::string fn = encodeRequestFilename("zz9", RequestType::Grpc, "", "Get List User");
    auto rt = parseRequestFilename(fn);
    CHECK_EQ(rt.id, std::string("zz9"), "round-trip keeps id");
    std::string label = normalizeDisplayName(rt.slug);
    CHECK(label.find("grpc") == std::string::npos && label.find("zz9") == std::string::npos,
          "label does NOT contain id/prefix");
    CHECK_EQ(label, std::string("Get list user"), "label = normalized name");
}

static void test_curl_export() {
    std::printf("[curl_export]\n");
    namespace d = core::domain;
    const d::RequestConfig cfg{d::Timeout::fromMillis(30000).take(), true};

    // HTTP: POST with a JSON body, a custom header, and a query param.
    std::vector<d::Header> hs;
    hs.push_back(d::Header::create("Content-Type", "application/json").take());
    hs.push_back(d::Header::create("X-Token", "abc123").take());
    std::vector<d::QueryParam> qp;
    qp.push_back(d::QueryParam::create("q", "hello").take());
    d::HttpRequest::Parts hp{d::HttpMethod::Post, d::Url::create("https://api.test/users").take(),
                             d::PathVariableList{}, d::QueryParamList{std::move(qp)},
                             d::HeaderList{std::move(hs)}, d::Body::raw(d::RawSubtype::Json, "{\"a\":1}"),
                             d::Auth::none()};
    auto httpModel = d::RequestModel::create(d::RequestId(""), "curl-http", 0, cfg,
                                             d::HttpRequest::create(std::move(hp)).take())
                         .take();
    std::string c = toCurl(httpModel);
    CHECK(c.find("curl -X POST") != std::string::npos, "has method");
    CHECK(c.find("api.test/users") != std::string::npos, "has url");
    CHECK(c.find("--data") != std::string::npos, "has body");
    CHECK(c.find("X-Token: abc123") != std::string::npos, "has X-Token header");
    CHECK(c.find("q=hello") != std::string::npos, "has param q");

    // gRPC: grpcurl form (tls off by config -> -plaintext).
    d::GrpcRequest::Parts gp;
    gp.target = "localhost:50051"; gp.service = "pkg.Svc"; gp.method = "M";
    gp.message = d::JsonText::of("{\"id\":\"1\"}");
    auto grpcModel = d::RequestModel::create(d::RequestId(""), "curl-grpc", 0,
                                             d::RequestConfig{d::Timeout::fromMillis(30000).take(), false},
                                             d::GrpcRequest::create(std::move(gp)).take())
                         .take();
    std::string gc = toCurl(grpcModel);
    CHECK(gc.find("grpcurl") != std::string::npos, "grpc -> grpcurl");
    CHECK(gc.find("-plaintext") != std::string::npos, "grpc tls off -> -plaintext");
    CHECK(gc.find("pkg.Svc/M") != std::string::npos, "has service/method");

    // Kafka: kcat form (regression: used to silently export "").
    {
        d::KafkaProduceConfig pc{d::KafkaTopic::create("demo-topic").take()};
        d::KafkaMessage msg;
        msg.key = d::MessageKey{"k1"};
        msg.value = d::MessagePayload{"{\"x\":1}"};
        auto req = d::KafkaRequest::create(d::BrokerList::parse("localhost:9092").take(),
                                           d::KafkaSecurity::plaintext(),
                                           d::KafkaRequest::Mode{d::KafkaProduceSpec{pc, msg}})
                       .take();
        auto m = d::RequestModel::create(d::RequestId(""), "curl-kafka", 0, cfg, std::move(req)).take();
        std::string kc = toCurl(m);
        CHECK(kc.find("kcat -P") != std::string::npos, "kafka producer -> kcat -P");
        CHECK(kc.find("localhost:9092") != std::string::npos, "kcat has brokers");
        CHECK(kc.find("demo-topic") != std::string::npos, "kcat has topic");
        CHECK(kc.find("-k 'k1'") != std::string::npos, "kcat has key");

        d::KafkaConsumeConfig cc{{d::KafkaTopic::create("demo-topic").take()}, std::nullopt,
                                 d::ConsumerGroup::create("g1").take()};
        auto creq = d::KafkaRequest::create(d::BrokerList::parse("localhost:9092").take(),
                                            d::KafkaSecurity::plaintext(),
                                            d::KafkaRequest::Mode{d::KafkaConsumeSpec{cc}})
                        .take();
        auto cm = d::RequestModel::create(d::RequestId(""), "curl-kafka-c", 0, cfg, std::move(creq)).take();
        std::string cc2 = toCurl(cm);
        CHECK(cc2.find("kcat") != std::string::npos, "kafka consumer -> kcat");
        CHECK(cc2.find("-G 'g1'") != std::string::npos, "kcat consumer group mode");
    }
}

int run_variables_export_tests() {
    test_variable_resolver();
    test_alias_inversion();
    test_curl_export();
    test_request_naming();
    std::printf("  variables_export: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
