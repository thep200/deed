// import_service_test.cpp — REFACTOR_SPEC P6: IImportService detect + parse into a DOMAIN RequestModel.
// Pure (no network). Verifies cURL/grpcurl/GraphQL are classified and parsed into the right domain type.
#include <cstdio>
#include <string>

#include "core/domain/request/request_model.hpp"
#include "infra/import/import_service.hpp"

using namespace core::domain;
using core::infra::ImportService;

static int im_pass = 0, im_fail = 0;
#define IM_CHECK(cond, msg)                                                                        \
  do {                                                                                             \
    if (cond) { ++im_pass; }                                                                       \
    else { ++im_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }             \
  } while (0)

int run_import_service_tests() {
  std::printf("[import_service]\n");
  ImportService svc;

  // cURL -> HTTP
  const std::string curl =
      "curl -X POST https://api.example.com/users -H 'Content-Type: application/json' -d '{\"a\":1}'";
  auto kCurl = svc.detect(curl);
  IM_CHECK(kCurl && *kCurl == ImportKind::Curl, "detect cURL");
  if (kCurl) {
    auto r = svc.import(curl, *kCurl);
    IM_CHECK(r.isOk(), "import cURL ok");
    if (r.isOk()) {
      IM_CHECK(r.value().model.type() == RequestType::Http, "cURL -> Http");
      r.value().model.match([&](auto &&p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, HttpRequest>) {
          IM_CHECK(p.method() == HttpMethod::Post, "cURL method POST");
          IM_CHECK(p.url().raw() == "https://api.example.com/users", "cURL url parsed");
        }
      });
    }
  }

  // grpcurl -> gRPC
  const std::string grpc = "grpcurl -plaintext localhost:50051 pkg.Service/Method";
  auto kGrpc = svc.detect(grpc);
  IM_CHECK(kGrpc && *kGrpc == ImportKind::Grpcurl, "detect grpcurl");
  if (kGrpc) {
    auto r = svc.import(grpc, *kGrpc);
    IM_CHECK(r.isOk() && r.value().model.type() == RequestType::Grpc, "grpcurl -> Grpc");
  }

  // GraphQL document -> GraphQL
  const std::string gql = "query Me { me { id name } }";
  auto kGql = svc.detect(gql);
  IM_CHECK(kGql && *kGql == ImportKind::GraphQl, "detect GraphQL");
  if (kGql) {
    auto r = svc.import(gql, *kGql);
    IM_CHECK(r.isOk() && r.value().model.type() == RequestType::GraphQl, "GraphQL -> GraphQl");
  }

  // Garbage -> no detection.
  IM_CHECK(!svc.detect("just some random text").has_value(), "non-command -> no detect");

  std::printf("  import_service: %d passed, %d failed\n", im_pass, im_fail);
  return im_fail;
}
