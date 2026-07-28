#include "infra/transport/kafka/avro_serde.hpp"

#include <sstream>

// BEFORE the avro headers: avro/Exception.hh calls fmt::format but only includes <fmt/core.h>, relying
// on core.h transitively providing it — true for some fmt versions, not others (CI broke on this).
#include <fmt/format.h>

#include <avro/Compiler.hh>
#include <avro/Decoder.hh>
#include <avro/Encoder.hh>
#include <avro/Generic.hh>
#include <avro/Specific.hh>
#include <avro/Stream.hh>
#include <avro/ValidSchema.hh>

namespace core::infra::avro_serde {
namespace d = core::domain;

std::optional<std::int32_t> extractConfluentSchemaId(const std::string &bytes) {
  if (bytes.size() < 5 || bytes[0] != '\x00') return std::nullopt;
  const auto *b = reinterpret_cast<const unsigned char *>(bytes.data());
  std::uint32_t u = (std::uint32_t(b[1]) << 24) | (std::uint32_t(b[2]) << 16) |
                    (std::uint32_t(b[3]) << 8) | std::uint32_t(b[4]);
  return static_cast<std::int32_t>(u); // signed 4-byte BE per Confluent wire format
}

std::string wrapConfluent(std::int32_t schemaId, const std::string &avroBinary) {
  std::string out;
  out.reserve(5 + avroBinary.size());
  out.push_back('\x00');
  auto u = static_cast<std::uint32_t>(schemaId);
  out.push_back(static_cast<char>((u >> 24) & 0xFF));
  out.push_back(static_cast<char>((u >> 16) & 0xFF));
  out.push_back(static_cast<char>((u >> 8) & 0xFF));
  out.push_back(static_cast<char>(u & 0xFF));
  out += avroBinary;
  return out;
}

d::Result<std::string> jsonToAvroBinary(const std::string &schemaJson, const std::string &jsonText) {
  try {
    avro::ValidSchema schema = avro::compileJsonSchemaFromString(schemaJson);

    // JSON text -> GenericDatum (jsonDecoder validates against the schema).
    avro::DecoderPtr jd = avro::jsonDecoder(schema);
    auto jin = avro::memoryInputStream(reinterpret_cast<const std::uint8_t *>(jsonText.data()),
                                       jsonText.size());
    jd->init(*jin);
    avro::GenericDatum datum(schema);
    avro::GenericReader reader(schema, jd);
    reader.read(datum);

    // GenericDatum -> Avro binary.
    std::ostringstream sink;
    auto out = avro::ostreamOutputStream(sink);
    avro::EncoderPtr be = avro::binaryEncoder();
    be->init(*out);
    avro::GenericWriter writer(schema, be);
    writer.write(datum);
    be->flush();
    out->flush();
    return d::Result<std::string>::ok(sink.str());
  } catch (const std::exception &e) {
    return d::Result<std::string>::fail({d::ErrorCode::Parse, e.what(), ""});
  }
}

d::Result<std::string> avroBinaryToJson(const std::string &schemaJson, const char *data,
                                        std::size_t len) {
  try {
    avro::ValidSchema schema = avro::compileJsonSchemaFromString(schemaJson);

    // Avro binary -> GenericDatum.
    avro::DecoderPtr bd = avro::binaryDecoder();
    auto bin = avro::memoryInputStream(reinterpret_cast<const std::uint8_t *>(data), len);
    bd->init(*bin);
    avro::GenericDatum datum(schema);
    avro::GenericReader reader(schema, bd);
    reader.read(datum);

    // GenericDatum -> JSON text (Avro-JSON encoding).
    std::ostringstream sink;
    auto out = avro::ostreamOutputStream(sink);
    avro::EncoderPtr je = avro::jsonEncoder(schema);
    je->init(*out);
    avro::GenericWriter writer(schema, je);
    writer.write(datum);
    je->flush();
    out->flush();
    return d::Result<std::string>::ok(sink.str());
  } catch (const std::exception &e) {
    return d::Result<std::string>::fail({d::ErrorCode::Parse, e.what(), ""});
  }
}

} // namespace core::infra::avro_serde
