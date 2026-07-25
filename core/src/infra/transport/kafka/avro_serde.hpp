// avro_serde.hpp — Confluent wire framing + Avro<->JSON serde (SPEC_kafka Avro v1). INTERNAL (core/src).
// Framing helpers are pure byte math (no avro-cpp); the serde pair wraps avro-cpp and stays in the .cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/domain/common/result.hpp"

namespace core::infra::avro_serde {

// Confluent wire format: byte 0 = magic 0x00, bytes 1-4 = schema id (4-byte SIGNED big-endian),
// rest = Avro binary. nullopt unless size >= 5 and magic matches.
std::optional<std::int32_t> extractConfluentSchemaId(const std::string &bytes);
std::string wrapConfluent(std::int32_t schemaId, const std::string &avroBinary);

// JSON text -> Avro binary against `schemaJson` (writer schema). Unions expect the Avro-JSON encoding
// ({"string": "x"}, null) — same convention as kafka-avro-console-producer. Any avro-cpp error ->
// Result::fail(Parse, what()).
core::domain::Result<std::string> jsonToAvroBinary(const std::string &schemaJson,
                                                   const std::string &jsonText);
// Avro binary -> JSON text (Avro-JSON encoding) against `schemaJson`.
core::domain::Result<std::string> avroBinaryToJson(const std::string &schemaJson, const char *data,
                                                   std::size_t len);

} // namespace core::infra::avro_serde
