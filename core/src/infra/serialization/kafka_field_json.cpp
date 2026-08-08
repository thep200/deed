#include "core/infra/serialization/field_json.hpp"

#include <chrono>

#include <nlohmann/json.hpp>

#include "infra/serialization/field_json_common.hpp"

namespace core::serial {
namespace d = core::domain;

std::string kafkaRecordToDisplayJson(const d::KafkaRecord &r) {
  json j;
  j["topic"] = r.topic;
  j["partition"] = r.partition;
  j["offset"] = r.offset;
  j["key"] = r.key ? json(*r.key) : json(nullptr);
  if (r.valueIsNull) {
    j["value"] = nullptr;
  } else {
    try {
      j["value"] = json::parse(r.value); // embed parsed JSON when possible
    } catch (...) {
      j["value"] = r.value; // else raw string
    }
  }
  if (!r.valueEncoding.empty()) j["valueEncoding"] = r.valueEncoding; // e.g. "avro (id 7)"
  json hs = json::array();
  for (const auto &h : r.headers)
    if (h.enabled) hs.push_back({{"key", h.key}, {"value", h.value}});
  j["headers"] = hs;
  j["timestampMs"] = r.timestampMs;
  j["size"] = r.size;
  // value/headers hold verbatim bytes — invalid UTF-8 must render as U+FFFD, not throw out of a noexcept observer.
  return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace {
// Schema Registry block ({url, username, password}); absent/empty url = not configured.
json schemaRegistryToJson(const d::SchemaRegistryRef &r) {
  return json{{"url", r.url}, {"username", r.username}, {"password", r.password}};
}
d::SchemaRegistryRef schemaRegistryFromJson(const json &j) {
  d::SchemaRegistryRef r;
  if (auto it = j.find("schemaRegistry"); it != j.end() && it->is_object()) {
    r.url = gs(*it, "url");
    r.username = gs(*it, "username");
    r.password = gs(*it, "password");
  }
  return r;
}
template <class T> json kafkaKvToJson(const std::vector<T> &list) {
  json a = json::array();
  for (const auto &e : list) a.push_back({{"key", e.key}, {"value", e.value}, {"enabled", e.enabled ? 1 : 0}});
  return a;
}
template <class T> std::vector<T> kafkaKvFromJson(const json &arr) {
  std::vector<T> out;
  if (arr.is_array())
    for (const auto &e : arr)
      if (e.is_object()) out.push_back(T{gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
  return out;
}
const char *kafkaAcksStr(d::Acks a) {
  switch (a) {
  case d::Acks::None: return "0";
  case d::Acks::Leader: return "1";
  case d::Acks::All: return "all";
  }
  return "all";
}
d::Acks kafkaAcksFrom(const std::string &s) {
  if (s == "0") return d::Acks::None;
  if (s == "1") return d::Acks::Leader;
  return d::Acks::All;
}
const char *kafkaCompressionStr(d::Compression c) {
  switch (c) {
  case d::Compression::None: return "none";
  case d::Compression::Gzip: return "gzip";
  case d::Compression::Snappy: return "snappy";
  case d::Compression::Lz4: return "lz4";
  case d::Compression::Zstd: return "zstd";
  }
  return "none";
}
d::Compression kafkaCompressionFrom(const std::string &s) {
  if (s == "gzip") return d::Compression::Gzip;
  if (s == "snappy") return d::Compression::Snappy;
  if (s == "lz4") return d::Compression::Lz4;
  if (s == "zstd") return d::Compression::Zstd;
  return d::Compression::None;
}
} // namespace

std::string kafkaMessageToJson(const d::KafkaMessage &m) {
  json j;
  j["key"] = m.key ? m.key->value : std::string();
  // Embed value as a real nested JSON value, not a JSON-encoded string — the producer value is always JSON.
  try {
    j["value"] = json::parse(m.value.value);
  } catch (...) {
    j["value"] = m.value.value; // not valid JSON yet (empty draft / mid-edit) -> show verbatim as a string
  }
  j["headers"] = kafkaKvToJson(m.headers);
  return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace); // mid-edit bytes must not throw
}
d::Result<d::KafkaMessage> jsonToKafkaMessage(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    d::KafkaMessage m;
    std::string key = gs(j, "key");
    if (!key.empty()) m.key = d::MessageKey{key};
    if (auto it = j.find("value"); it != j.end()) {
      // Real JSON -> re-serialize; plain string -> as-is (mid-edit drafts are stored verbatim).
      m.value.value = it->is_string() ? it->get<std::string>() : it->dump(2);
    }
    m.headers = kafkaKvFromJson<d::KafkaHeader>(j.value("headers", json::array()));
    return d::Result<d::KafkaMessage>::ok(std::move(m));
  } catch (const std::exception &e) {
    return parseErr<d::KafkaMessage>(e.what());
  }
}

std::string kafkaProduceConfigToJson(const d::KafkaProduceConfig &c) {
  json j{{"topic", c.topic.value()},
        {"partition", c.partition.value},
        {"acks", kafkaAcksStr(c.acks)},
        {"compression", kafkaCompressionStr(c.compression)},
        {"messageTimeoutMs", c.messageTimeout.count()},
        {"lingerMs", c.linger.count()},
        {"retries", c.retries},
        {"idempotence", c.idempotence},
        {"clientId", c.clientId},
        // Always emitted (defaults included) so the keys are discoverable in the Kafka tab.
        {"valueFormat", c.valueFormat == d::KafkaValueFormat::Avro ? "avro" : "json"},
        {"schemaRegistry", schemaRegistryToJson(c.schemaRegistry)},
        {"extra", kafkaKvToJson(c.extra)}};
  return j.dump(2);
}
d::Result<d::KafkaProduceConfig> jsonToKafkaProduceConfig(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    auto topic = d::KafkaTopic::create(gs(j, "topic"));
    if (!topic) return d::Result<d::KafkaProduceConfig>::fail(topic.error());
    d::KafkaProduceConfig c{topic.take()};
    c.partition = d::KafkaPartition{gi(j, "partition", d::KafkaPartition::kAuto)};
    c.acks = kafkaAcksFrom(gs(j, "acks", "all"));
    c.compression = kafkaCompressionFrom(gs(j, "compression", "none"));
    c.messageTimeout = std::chrono::milliseconds(gi(j, "messageTimeoutMs", 30000));
    c.linger = std::chrono::milliseconds(gi(j, "lingerMs", 0));
    c.retries = gi(j, "retries", 3);
    c.idempotence = gb(j, "idempotence", false);
    c.clientId = gs(j, "clientId", "deed");
    c.valueFormat = gs(j, "valueFormat", "json") == "avro" ? d::KafkaValueFormat::Avro
                                                           : d::KafkaValueFormat::Json;
    c.schemaRegistry = schemaRegistryFromJson(j); // absent -> not configured
    c.extra = kafkaKvFromJson<d::KafkaExtra>(j.value("extra", json::array()));
    return d::Result<d::KafkaProduceConfig>::ok(std::move(c));
  } catch (const std::exception &e) {
    return parseErr<d::KafkaProduceConfig>(e.what());
  }
}

std::string kafkaConsumeConfigToJson(const d::KafkaConsumeConfig &c) {
  json topics = json::array();
  for (const auto &t : c.topics) topics.push_back(t.value());
  json j{{"topics", topics},
        {"group", c.group.value()},
        {"offsetReset", c.offsetReset == d::OffsetReset::Earliest ? "earliest" : "latest"},
        {"partition", c.partition ? c.partition->value : d::KafkaPartition::kAuto},
        {"autoCommit", c.autoCommit},
        {"maxMessages", c.maxMessages ? json(*c.maxMessages) : json(nullptr)},
        {"pollTimeoutMs", c.pollTimeout.count()},
        {"clientId", c.clientId},
        {"schemaRegistry", schemaRegistryToJson(c.schemaRegistry)},
        {"extra", kafkaKvToJson(c.extra)}};
  return j.dump(2);
}
d::Result<d::KafkaConsumeConfig> jsonToKafkaConsumeConfig(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    std::vector<d::KafkaTopic> topics;
    if (auto it = j.find("topics"); it != j.end() && it->is_array())
      for (const auto &e : *it)
        if (e.is_string()) {
          auto t = d::KafkaTopic::create(e.get<std::string>());
          if (!t) return d::Result<d::KafkaConsumeConfig>::fail(t.error());
          topics.push_back(t.take());
        }
    auto group = d::ConsumerGroup::create(gs(j, "group"));
    if (!group) return d::Result<d::KafkaConsumeConfig>::fail(group.error());
    int partitionRaw = gi(j, "partition", d::KafkaPartition::kAuto);
    std::optional<d::KafkaPartition> partition;
    if (partitionRaw != d::KafkaPartition::kAuto) partition = d::KafkaPartition{partitionRaw};
    d::KafkaConsumeConfig c{std::move(topics), partition, group.take()};
    c.offsetReset = gs(j, "offsetReset", "latest") == "earliest" ? d::OffsetReset::Earliest : d::OffsetReset::Latest;
    c.autoCommit = gb(j, "autoCommit", true);
    if (auto it = j.find("maxMessages"); it != j.end() && it->is_number()) c.maxMessages = it->get<int>();
    c.pollTimeout = std::chrono::milliseconds(gi(j, "pollTimeoutMs", 500));
    c.clientId = gs(j, "clientId", "deed");
    c.schemaRegistry = schemaRegistryFromJson(j);
    c.extra = kafkaKvFromJson<d::KafkaExtra>(j.value("extra", json::array()));
    return d::Result<d::KafkaConsumeConfig>::ok(std::move(c));
  } catch (const std::exception &e) {
    return parseErr<d::KafkaConsumeConfig>(e.what());
  }
}

// Kafka config carries no "tls" key.
std::string kafkaRequestConfigToJson(const d::RequestConfig &c) {
  json j;
  j["timeout_ms"] = c.timeout.millis();
  return j.dump(2);
}
d::Result<d::RequestConfig> jsonToKafkaRequestConfig(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    if (!j.is_object()) return parseErr<d::RequestConfig>("config must be a JSON object");
    long long ms = 30000;
    if (auto it = j.find("timeout_ms"); it != j.end() && it->is_number()) ms = it->get<long long>();
    auto t = d::Timeout::fromMillis(ms);
    if (!t) return d::Result<d::RequestConfig>::fail(t.error());
    return d::Result<d::RequestConfig>::ok(d::RequestConfig{t.take(), false}); // Kafka: no TLS toggle yet
  } catch (const std::exception &e) {
    return parseErr<d::RequestConfig>(e.what());
  }
}

} // namespace core::serial
