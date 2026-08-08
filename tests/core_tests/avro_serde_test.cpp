// Pure fixtures, no network/broker.
#include <cstdio>
#include <string>

#include "infra/transport/kafka/avro_serde.hpp"
#include "infra/transport/kafka/schema_registry_client.hpp"

using namespace core;
namespace serde = core::infra::avro_serde;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char *msg) {
  if (ok) ++g_pass;
  else { ++g_fail; std::printf("  FAIL[avro_serde]: %s\n", msg); }
}
bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

// Fixture writer schema: primitives + nullable union + enum + array<double>.
const char *kSchema = R"({
  "type": "record", "name": "Signal", "fields": [
    {"name": "name", "type": "string"},
    {"name": "count", "type": "long"},
    {"name": "note", "type": ["null", "string"], "default": null},
    {"name": "level", "type": {"type": "enum", "name": "Level", "symbols": ["LOW", "HIGH"]}},
    {"name": "samples", "type": {"type": "array", "items": "double"}}
  ]})";
} // namespace

int run_avro_serde_tests() {
  // Confluent framing: 1-byte magic 0x00 + 4-byte SIGNED big-endian id.
  {
    std::string wire = serde::wrapConfluent(7, "PAYLOAD");
    check(wire.size() == 5 + 7, "framing: 5-byte prefix");
    check(wire[0] == '\x00', "framing: magic byte");
    check(wire[1] == 0 && wire[2] == 0 && wire[3] == 0 && wire[4] == 7, "framing: id 7 big-endian");
    auto id = serde::extractConfluentSchemaId(wire);
    check(id && *id == 7, "framing: id round-trip");

    std::string big = serde::wrapConfluent(0x01020304, "");
    check((unsigned char)big[1] == 0x01 && (unsigned char)big[2] == 0x02 &&
              (unsigned char)big[3] == 0x03 && (unsigned char)big[4] == 0x04,
          "framing: byte order is big-endian");
    auto negId = serde::extractConfluentSchemaId(serde::wrapConfluent(-2, "x"));
    check(negId && *negId == -2, "framing: signed id round-trip");

    check(!serde::extractConfluentSchemaId(""), "framing: empty rejected");
    check(!serde::extractConfluentSchemaId(std::string("\x00\x00\x00", 3)), "framing: <5 bytes rejected");
    check(!serde::extractConfluentSchemaId("{\"json\": 1}"), "framing: wrong magic rejected");
  }

  // Serde round-trip against the fixture schema (union uses the Avro-JSON encoding).
  {
    const std::string json =
        R"({"name": "a", "count": 3, "note": {"string": "hi"}, "level": "HIGH", "samples": [1.5, 2.5]})";
    auto bin = serde::jsonToAvroBinary(kSchema, json);
    check(bin.isOk(), "serde: JSON -> Avro binary");
    if (bin.isOk()) {
      auto back = serde::avroBinaryToJson(kSchema, bin.value().data(), bin.value().size());
      check(back.isOk(), "serde: Avro binary -> JSON");
      if (back.isOk()) {
        check(has(back.value(), "\"a\"") && has(back.value(), "3"), "serde: primitives survive");
        check(has(back.value(), "{\"string\":") || has(back.value(), "{ \"string\":"),
              "serde: union keeps Avro-JSON encoding");
        check(has(back.value(), "HIGH"), "serde: enum survives");
      }
    }
    auto nullNote = serde::jsonToAvroBinary(
        kSchema, R"({"name": "b", "count": 0, "note": null, "level": "LOW", "samples": []})");
    check(nullNote.isOk(), "serde: null union branch accepted");

    check(!serde::jsonToAvroBinary(kSchema, "not json").isOk(), "serde: non-JSON input fails");
    check(!serde::jsonToAvroBinary(kSchema, R"({"name": "x"})").isOk(),
          "serde: missing fields fail");
    check(!serde::jsonToAvroBinary("{bad schema", "{}").isOk(), "serde: bad schema fails");
    check(!serde::avroBinaryToJson(kSchema, "\x01\x02", 2).isOk(), "serde: garbage binary fails");
    // Bare (non-Avro-JSON) union input: no pinned verdict — just document jsonDecoder's behavior either way.
    auto bare = serde::jsonToAvroBinary(
        kSchema, R"({"name": "c", "count": 1, "note": "bare", "level": "LOW", "samples": []})");
    std::printf("  note: bare union input %s\n", bare.isOk() ? "ACCEPTED" : "rejected (Avro-JSON required)");
  }

  // Schema Registry response parsing.
  {
    auto latest = infra::parseLatestSubjectResponse(
        R"({"subject":"t-value","version":3,"id":42,"schema":"{\"type\":\"string\"}"})");
    check(latest.isOk() && latest.value().id == 42, "sr: latest id");
    check(latest.isOk() && latest.value().schemaJson == "{\"type\":\"string\"}",
          "sr: schema unescaped to plain JSON text");
    check(!infra::parseLatestSubjectResponse(R"({"schema":"{}"})").isOk(), "sr: latest without id fails");
    auto byId = infra::parseSchemaByIdResponse(R"({"schema":"{\"type\":\"long\"}"})");
    check(byId.isOk() && byId.value() == "{\"type\":\"long\"}", "sr: by-id schema");
    check(!infra::parseSchemaByIdResponse(R"({"error_code":40403})").isOk(), "sr: error body fails");
    check(!infra::parseSchemaByIdResponse("<html>").isOk(), "sr: non-JSON fails");
  }

  std::printf("  avro_serde: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
