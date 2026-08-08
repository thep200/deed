#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/domain/kafka/kafka_common.hpp"

namespace core::domain {

enum class Acks { None, Leader, All };               // "0" / "1" / "all"
enum class Compression { None, Gzip, Snappy, Lz4, Zstd };
// Wire format: verbatim JSON text, or Confluent-framed Avro against the LATEST `<topic>-value` registry schema.
enum class KafkaValueFormat { Json, Avro };

// New-request defaults: seed a fresh request's config; per-request, not app-global .env tunables.
inline constexpr std::chrono::milliseconds kDefaultMessageTimeout{30000};
inline constexpr std::chrono::milliseconds kDefaultLinger{0};
inline constexpr int kDefaultProduceRetries = 3;
inline constexpr const char *kDefaultKafkaClientId = "deed";

// Plain aggregate — invariants are checked by KafkaRequest::create.
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
// Always JSON text, validated at the use-case layer (IJsonValidator), never here; no raw/binary mode.
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
