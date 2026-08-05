// field_json_test.cpp — round-trip + validation tests for core::serial (domain field codec, REFACTOR_SPEC
// Phase E enabler): JSON <-> domain Header/QueryParam/Auth/Body/Config. Mirrors the legacy fieldcodec gate.
#include <cstdio>
#include <string>

#include "core/infra/serialization/field_json.hpp"

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
    // Flat shape is what we WRITE (one "type" key, fields at top level — no per-type sub-object).
    check(serial::authToJson(basic).find("\"basic\": {") == std::string::npos &&
              serial::authToJson(basic).find("\"username\"") != std::string::npos,
          "auth json is flat (no nested sub-object)");
    // Only the flat shape is read — no per-type sub-object, no removed types.
    check(!serial::jsonToAuth(R"({"type":"basic","basic":{"username":"u","password":"p"}})").isOk(),
          "nested auth shape rejected");
    check(!serial::jsonToAuth(R"({"type":"apikey","key":"k"})").isOk(), "unknown auth type rejected");

    // OAuth2: both grants round-trip; defaults fill; unknown enum strings rejected.
    AuthOAuth2 occ;
    occ.tokenUrl = "https://idp/token"; occ.clientId = "cid"; occ.clientSecret = "s"; occ.scope = "r";
    Auth oauthCc = Auth::oauth2(occ).take();
    auto rcc = serial::jsonToAuth(serial::authToJson(oauthCc));
    check(rcc.isOk() && rcc.value() == oauthCc, "auth oauth2 (client_credentials) round-trip");
    AuthOAuth2 opw = occ;
    opw.grant = OAuth2Grant::Password; opw.username = "u"; opw.password = "p";
    opw.clientAuth = OAuth2ClientAuth::Body;
    Auth oauthPw = Auth::oauth2(opw).take();
    auto rpw = serial::jsonToAuth(serial::authToJson(oauthPw));
    check(rpw.isOk() && rpw.value() == oauthPw, "auth oauth2 (password/body) round-trip");
    auto defs = serial::jsonToAuth(R"({"type":"oauth2","tokenUrl":"https://t","clientId":"c"})");
    check(defs.isOk(), "oauth2 minimal parses (defaults)");
    check(!serial::jsonToAuth(R"({"type":"oauth2","tokenUrl":"t","clientId":"c","grant":"implicit"})").isOk(),
          "oauth2 unknown grant rejected");
    check(!serial::jsonToAuth(R"({"type":"oauth2","tokenUrl":"t","clientId":"c","clientAuth":"query"})").isOk(),
          "oauth2 unknown clientAuth rejected");
  }

  // Kafka record display: valueEncoding annotation + non-UTF8 must never throw (it renders inside a
  // noexcept observer — a throw is std::terminate; regression for the Avro-binary-value crash).
  {
    KafkaRecord r;
    r.topic = "t";
    r.value = "{\"a\":1}";
    std::string plain = serial::kafkaRecordToDisplayJson(r);
    check(plain.find("valueEncoding") == std::string::npos, "record display: no encoding key when plain");
    r.valueEncoding = "avro (id 7)";
    check(serial::kafkaRecordToDisplayJson(r).find("avro (id 7)") != std::string::npos,
          "record display: valueEncoding shown");
    KafkaRecord bin;
    bin.topic = "t";
    bin.value = std::string("\x00\x01\xff\xfe", 4); // invalid UTF-8 + NULs
    std::string out = serial::kafkaRecordToDisplayJson(bin);
    check(!out.empty(), "record display: non-UTF8 bytes render (no throw)");
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
