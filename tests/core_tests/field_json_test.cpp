// field_json_test.cpp — round-trip + validation tests for core::serial (domain field codec, REFACTOR_SPEC
// Phase E enabler): JSON <-> domain Header/QueryParam/Auth/Body/Config. Mirrors the legacy fieldcodec gate.
#include <cstdio>
#include <string>

#include "core/serialization/field_json.hpp"

using namespace core;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char *msg) {
  if (ok) ++g_pass;
  else { ++g_fail; std::printf("  FAIL[field_json]: %s\n", msg); }
}
} // namespace

int run_field_json_tests() {
  using namespace core::domain;

  // Headers: round-trip (order + enabled preserved) + validation error on bad name.
  {
    std::vector<Header> hs;
    hs.push_back(Header::create("X-A", "1", true).take());
    hs.push_back(Header::create("X-B", "2", false).take());
    HeaderList list(std::move(hs));
    auto rt = serial::jsonToHeaders(serial::headersToJson(list));
    check(rt.isOk() && rt.value() == list, "headers round-trip");
    auto bad = serial::jsonToHeaders("[{\"key\":\"bad header\",\"value\":\"v\",\"enabled\":1}]");
    check(!bad.isOk(), "invalid header name rejected");
    auto empty = serial::jsonToHeaders("");
    check(empty.isOk() && empty.value().empty(), "empty headers -> empty list");
  }

  // Params: round-trip.
  {
    std::vector<QueryParam> ps;
    ps.push_back(QueryParam::create("a", "1", true).take());
    ps.push_back(QueryParam::create("b", "", true).take());
    QueryParamList list(std::move(ps));
    auto rt = serial::jsonToParams(serial::paramsToJson(list));
    check(rt.isOk() && rt.value() == list, "params round-trip");
  }

  // Auth: each alternative round-trips.
  {
    auto none = serial::jsonToAuth(serial::authToJson(Auth::none()));
    check(none.isOk() && none.value() == Auth::none(), "auth none round-trip");
    Auth basic = Auth::basic("u", "p").take();
    auto rb = serial::jsonToAuth(serial::authToJson(basic));
    check(rb.isOk() && rb.value() == basic, "auth basic round-trip");
    Auth bearer = Auth::bearer("tok").take();
    auto rbe = serial::jsonToAuth(serial::authToJson(bearer));
    check(rbe.isOk() && rbe.value() == bearer, "auth bearer round-trip");
    Auth apik = Auth::apiKey("k", "v", ApiKeyIn::Query).take();
    auto ra = serial::jsonToAuth(serial::authToJson(apik));
    check(ra.isOk() && ra.value() == apik, "auth apikey(query) round-trip");
  }

  // Body: each variant round-trips.
  {
    Body raw = Body::raw(RawSubtype::Json, "{\"k\":1}");
    auto rr = serial::jsonToBody(serial::bodyToJson(raw));
    check(rr.isOk() && rr.value() == raw, "body raw(json) round-trip");
    Body form = Body::formUrlEncoded({{"a", "1", true}, {"b", "2", false}});
    auto rf = serial::jsonToBody(serial::bodyToJson(form));
    check(rf.isOk() && rf.value() == form, "body form-urlencoded round-trip");
    Body mp = Body::multipart({{"f", PartKind::File, "", "/tmp/x", true}}).take();
    auto rm = serial::jsonToBody(serial::bodyToJson(mp));
    check(rm.isOk() && rm.value() == mp, "body multipart round-trip");
    Body bin = Body::binary("/tmp/blob").take();
    auto rbin = serial::jsonToBody(serial::bodyToJson(bin));
    check(rbin.isOk() && rbin.value() == bin, "body binary round-trip");
    auto rn = serial::jsonToBody(serial::bodyToJson(Body::none()));
    check(rn.isOk() && rn.value() == Body::none(), "body none round-trip");
  }

  // Config: round-trip timeout + tls.
  {
    RequestConfig c{Timeout::fromMillis(12345).take(), false};
    auto rt = serial::jsonToConfig(serial::configToJson(c));
    check(rt.isOk() && rt.value() == c, "config round-trip");
  }

  std::printf("[field_json] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
