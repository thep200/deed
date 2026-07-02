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

// Invariant (partition -1(auto) or >=0) is checked by KafkaRequest::create (kafka_request.hpp), which sees
// the whole Mode variant at once — this struct itself is a plain aggregate, like the spec's pseudocode.
struct KafkaProduceConfig {
  KafkaTopic topic;
  KafkaPartition partition{KafkaPartition::kAuto};
  Acks acks = Acks::All;
  Compression compression = Compression::None;
  std::chrono::milliseconds messageTimeout{30000};
  std::chrono::milliseconds linger{0};
  int retries = 3;
  bool idempotence = false;
  std::string clientId = "deed";
  std::vector<KafkaExtra> extra;

  bool operator==(const KafkaProduceConfig &o) const {
    return topic == o.topic && partition == o.partition && acks == o.acks && compression == o.compression &&
           messageTimeout == o.messageTimeout && linger == o.linger && retries == o.retries &&
           idempotence == o.idempotence && clientId == o.clientId && extra == o.extra;
  }
  bool operator!=(const KafkaProduceConfig &o) const { return !(*this == o); }
};

struct MessageKey {
  std::string value;
  bool operator==(const MessageKey &o) const { return value == o.value; }
};
// A producer value is always JSON text (validated by IJsonValidator at the use-case layer, not here — domain
// stays JSON-agnostic); tombstone means "no value" and skips validation. No raw/binary mode.
struct MessagePayload {
  std::string value;
  bool tombstone = false;
  bool operator==(const MessagePayload &o) const {
    return value == o.value && tombstone == o.tombstone;
  }
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
