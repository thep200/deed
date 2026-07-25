// core/domain/kafka/kafka_produce.hpp — producer config + message VOs (SPEC_kafka §3, tab Config/Message).
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/domain/kafka/kafka_common.hpp"

namespace core::domain {

enum class Acks { None, Leader, All };               // "0" / "1" / "all"
enum class Compression { None, Gzip, Snappy, Lz4, Zstd };
// How the (always-JSON) editor value goes on the wire: verbatim JSON text, or Avro binary in the
// Confluent framing, serialized against the LATEST Schema Registry schema of `<topic>-value`.
enum class KafkaValueFormat { Json, Avro };

// New-request producer defaults (SPEC_kafka §3). Pure domain — these seed a freshly created request's
// Config tab; the user edits them per request in the UI (they are not app-global .env tunables).
inline constexpr std::chrono::milliseconds kDefaultMessageTimeout{30000};
inline constexpr std::chrono::milliseconds kDefaultLinger{0};
inline constexpr int kDefaultProduceRetries = 3;
inline constexpr const char *kDefaultKafkaClientId = "deed";

// Invariant (partition -1(auto) or >=0) is checked by KafkaRequest::create (kafka_request.hpp), which sees
// the whole Mode variant at once — this struct itself is a plain aggregate, like the spec's pseudocode.
struct KafkaProduceConfig {
  KafkaTopic topic;
  KafkaPartition partition{KafkaPartition::kAuto};
  Acks acks = Acks::All;
  Compression compression = Compression::None;
  std::chrono::milliseconds messageTimeout{kDefaultMessageTimeout};
  std::chrono::milliseconds linger{kDefaultLinger};
  int retries = kDefaultProduceRetries;
  bool idempotence = false;
  std::string clientId = kDefaultKafkaClientId;
  KafkaValueFormat valueFormat = KafkaValueFormat::Json; // Avro needs schemaRegistry (checked at send)
  SchemaRegistryRef schemaRegistry;
  std::vector<KafkaExtra> extra;

  bool operator==(const KafkaProduceConfig &o) const {
    return topic == o.topic && partition == o.partition && acks == o.acks && compression == o.compression &&
           messageTimeout == o.messageTimeout && linger == o.linger && retries == o.retries &&
           idempotence == o.idempotence && clientId == o.clientId && valueFormat == o.valueFormat &&
           schemaRegistry == o.schemaRegistry && extra == o.extra;
  }
  bool operator!=(const KafkaProduceConfig &o) const { return !(*this == o); }
};

struct MessageKey {
  std::string value;
  bool operator==(const MessageKey &o) const { return value == o.value; }
};
// A producer value is always JSON text (validated by IJsonValidator at the use-case layer, not here — domain
// stays JSON-agnostic). No raw/binary mode.
struct MessagePayload {
  std::string value;
  bool operator==(const MessagePayload &o) const { return value == o.value; }
};
struct KafkaMessage {
  std::optional<MessageKey> key;
  MessagePayload value;
  std::vector<KafkaHeader> headers;
  bool operator==(const KafkaMessage &o) const {
    return key == o.key && value == o.value && headers == o.headers;
  }
};

struct KafkaProduceSpec {
  KafkaProduceConfig config;
  KafkaMessage message;
  bool operator==(const KafkaProduceSpec &o) const { return config == o.config && message == o.message; }
};

} // namespace core::domain
