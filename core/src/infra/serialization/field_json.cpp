#include "core/infra/serialization/field_json.hpp"

#include <chrono>

#include <nlohmann/json.hpp>

#include "infra/serialization/json_codec.hpp"  // core::codec::parseGuarded — JSON nesting-depth guard (H5)
#include "infra/serialization/wire_format.hpp" // core::wire::* on-disk body-mode tokens

namespace core::serial {
namespace d = core::domain;
namespace w = core::wire;
using nlohmann::json;

namespace {
std::string gs(const json &j, const char *k, const std::string &dflt = "") {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : dflt;
}
bool gb(const json &j, const char *k, bool dflt) {
  auto it = j.find(k);
  if (it == j.end()) return dflt;
  if (it->is_boolean()) return it->get<bool>();
  if (it->is_number()) return it->get<double>() != 0; // accept 0/1
  return dflt;
}
int gi(const json &j, const char *k, int dflt) {
  auto it = j.find(k);
  return (it != j.end() && it->is_number_integer()) ? it->get<int>() : dflt;
}
json parseGuarded(const std::string &text, const char *fallback) {
  return core::codec::parseGuarded(text.empty() ? fallback : text); // depth-guarded (H5)
}
template <class T> d::Result<T> parseErr(const std::string &what) {
  return d::Result<T>::fail({d::ErrorCode::Parse, what, ""});
}
// Raw-body subtype -> its wire "mode" token (and the key its text is stored under).
const char *rawSubtypeKey(d::RawSubtype s) {
  switch (s) {
  case d::RawSubtype::Text: return w::kBodyText;
  case d::RawSubtype::Xml: return w::kBodyXml;
  case d::RawSubtype::Json: return w::kBodyJson;
  }
  return w::kBodyJson;
}
// Body sub-collection parsers — keep jsonToBody a flat mode-dispatch (clang-tidy function-size).
std::vector<d::FormField> formFieldsFrom(const json &j) {
  std::vector<d::FormField> fields;
  if (auto it = j.find("formUrlEncoded"); it != j.end() && it->is_array())
    for (const auto &e : *it) fields.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
  return fields;
}
std::vector<d::MultipartPart> multipartPartsFrom(const json &j) {
  std::vector<d::MultipartPart> parts;
  if (auto it = j.find("multipart"); it != j.end() && it->is_array())
    for (const auto &e : *it) {
      d::MultipartPart p;
      p.key = gs(e, "key");
      p.value = gs(e, "value");
      p.kind = gs(e, "type", "text") == "file" ? d::PartKind::File : d::PartKind::Text;
      p.filePath = gs(e, "filePath");
      p.enabled = gb(e, "enabled", true);
      parts.push_back(std::move(p));
    }
  return parts;
}
} // namespace

// ---- Headers ----
std::string headersToJson(const d::HeaderList &list) {
  json a = json::array();
  for (const auto &h : list.items())
    a.push_back({{"key", h.name()}, {"value", h.value()}, {"enabled", h.enabled() ? 1 : 0}});
  return a.dump(2);
}
d::Result<d::HeaderList> jsonToHeaders(const std::string &text) {
  try {
    auto j = parseGuarded(text, "[]");
    if (!j.is_array()) return parseErr<d::HeaderList>("headers must be a JSON array");
    std::vector<d::Header> items;
    for (const auto &e : j) {
      if (!e.is_object()) continue;
      auto h = d::Header::create(gs(e, "key"), gs(e, "value"), gb(e, "enabled", true));
      if (!h) return d::Result<d::HeaderList>::fail(h.error());
      items.push_back(h.take());
    }
    return d::Result<d::HeaderList>::ok(d::HeaderList(std::move(items)));
  } catch (const std::exception &e) {
    return parseErr<d::HeaderList>(e.what());
  }
}

// ---- Query params ----
std::string paramsToJson(const d::QueryParamList &list) {
  json a = json::array();
  for (const auto &p : list.items())
    a.push_back({{"key", p.key()}, {"value", p.value()}, {"enabled", p.enabled() ? 1 : 0}});
  return a.dump(2);
}
d::Result<d::QueryParamList> jsonToParams(const std::string &text) {
  try {
    auto j = parseGuarded(text, "[]");
    if (!j.is_array()) return parseErr<d::QueryParamList>("params must be a JSON array");
    std::vector<d::QueryParam> items;
    for (const auto &e : j) {
      if (!e.is_object()) continue;
      auto p = d::QueryParam::create(gs(e, "key"), gs(e, "value"), gb(e, "enabled", true));
      if (!p) return d::Result<d::QueryParamList>::fail(p.error());
      items.push_back(p.take());
    }
    return d::Result<d::QueryParamList>::ok(d::QueryParamList(std::move(items)));
  } catch (const std::exception &e) {
    return parseErr<d::QueryParamList>(e.what());
  }
}

// ---- Auth ----
std::string authToJson(const d::Auth &auth) {
  json j;
  auth.match([&](auto &&a) {
    using T = std::decay_t<decltype(a)>;
    if constexpr (std::is_same_v<T, d::AuthNone>) {
      j["type"] = "none";
    } else if constexpr (std::is_same_v<T, d::AuthBasic>) {
      j["type"] = "basic";
      j["basic"] = {{"username", a.username}, {"password", a.password}};
    } else if constexpr (std::is_same_v<T, d::AuthBearer>) {
      j["type"] = "bearer";
      j["bearer"] = {{"token", a.token}};
    } else if constexpr (std::is_same_v<T, d::AuthApiKey>) {
      j["type"] = "apikey";
      j["apikey"] = {{"key", a.key}, {"value", a.value},
                     {"in", a.in == d::ApiKeyIn::Query ? "query" : "header"}};
    }
  });
  return j.dump(2);
}
d::Result<d::Auth> jsonToAuth(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{\"type\":\"none\"}");
    if (!j.is_object()) return parseErr<d::Auth>("auth must be a JSON object");
    std::string type = gs(j, "type", "none");
    if (type == "none") return d::Result<d::Auth>::ok(d::Auth::none());
    if (type == "basic") {
      auto b = j.contains("basic") ? j["basic"] : json::object();
      return d::Auth::basic(gs(b, "username"), gs(b, "password"));
    }
    if (type == "bearer") {
      auto b = j.contains("bearer") ? j["bearer"] : json::object();
      return d::Auth::bearer(gs(b, "token"));
    }
    if (type == "apikey") {
      auto b = j.contains("apikey") ? j["apikey"] : json::object();
      auto in = gs(b, "in", "header") == "query" ? d::ApiKeyIn::Query : d::ApiKeyIn::Header;
      return d::Auth::apiKey(gs(b, "key"), gs(b, "value"), in);
    }
    return parseErr<d::Auth>("unknown auth.type: " + type);
  } catch (const std::exception &e) {
    return parseErr<d::Auth>(e.what());
  }
}

// ---- Body ----
std::string bodyToJson(const d::Body &body) {
  json j;
  body.match([&](auto &&b) {
    using T = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<T, d::BodyNone>) {
      j["mode"] = w::kBodyNone;
    } else if constexpr (std::is_same_v<T, d::BodyRaw>) {
      const char *mode = rawSubtypeKey(b.subtype);
      j["mode"] = mode;
      j[mode] = b.text;
    } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
      j["mode"] = w::kBodyForm;
      json a = json::array();
      for (const auto &f : b.fields)
        a.push_back({{"key", f.key}, {"value", f.value}, {"enabled", f.enabled ? 1 : 0}});
      j["formUrlEncoded"] = a;
    } else if constexpr (std::is_same_v<T, d::BodyMultipart>) {
      j["mode"] = w::kBodyMultipart;
      json a = json::array();
      for (const auto &p : b.parts)
        a.push_back({{"key", p.key},
                     {"value", p.value},
                     {"type", p.kind == d::PartKind::File ? "file" : "text"},
                     {"filePath", p.filePath},
                     {"enabled", p.enabled ? 1 : 0}});
      j["multipart"] = a;
    } else if constexpr (std::is_same_v<T, d::BodyBinary>) {
      j["mode"] = w::kBodyBinary;
      j["binary"] = {{"filePath", b.filePath}};
    }
  });
  return j.dump(2);
}
d::Result<d::Body> jsonToBody(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{\"mode\":\"none\"}");
    if (!j.is_object()) return parseErr<d::Body>("body must be a JSON object");
    std::string mode = gs(j, "mode", w::kBodyNone);
    if (mode == w::kBodyJson) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Json, gs(j, w::kBodyJson)));
    if (mode == w::kBodyText) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Text, gs(j, w::kBodyText)));
    if (mode == w::kBodyXml) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Xml, gs(j, w::kBodyXml)));
    if (mode == w::kBodyForm)
      return d::Result<d::Body>::ok(d::Body::formUrlEncoded(formFieldsFrom(j)));
    if (mode == w::kBodyMultipart || mode == w::kBodyFormData)
      return d::Body::multipart(multipartPartsFrom(j));
    if (mode == w::kBodyBinary) {
      std::string fp;
      if (auto it = j.find(w::kBodyBinary); it != j.end() && it->is_object()) fp = gs(*it, "filePath");
      if (fp.empty()) return d::Result<d::Body>::ok(d::Body::none()); // binary chosen, no file yet
      return d::Body::binary(fp);
    }
    return d::Result<d::Body>::ok(d::Body::none());
  } catch (const std::exception &e) {
    return parseErr<d::Body>(e.what());
  }
}

// ---- Editor body view (unwrapped per-mode) ----
EditorBody bodyToEditor(const d::Body &body) {
  EditorBody eb{w::kBodyJson, ""};
  body.match([&](auto &&b) {
    using T = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<T, d::BodyRaw>) {
      eb.mode = rawSubtypeKey(b.subtype);
      eb.content = b.text;
    } else if constexpr (std::is_same_v<T, d::BodyFormUrlEncoded>) {
      eb.mode = w::kBodyForm;
      json a = json::array();
      for (const auto &f : b.fields)
        a.push_back({{"key", f.key}, {"value", f.value}, {"enabled", f.enabled ? 1 : 0}});
      eb.content = a.dump(2);
    } else if constexpr (std::is_same_v<T, d::BodyBinary>) {
      eb.mode = w::kBodyBinary;
      eb.content = json({{"filePath", b.filePath}}).dump(2);
    }
    // BodyNone / BodyMultipart -> default {"json",""} (the editor has no multipart mode).
  });
  return eb;
}
// Binary mode's editor content is either {"filePath"|"path": "..."} / a bare JSON string / or a plain path
// typed as-is — extracted out of bodyFromEditor (Step 3 flatten: was a 3rd nesting level inside it, try ->
// if/else-if, for a single mode branch) so the dispatcher below stays a flat sequence of guard clauses.
static std::string binaryFilePathFromEditor(const std::string &content) {
  try {
    auto j = json::parse(content);
    if (j.is_object()) return j.contains("filePath") ? gs(j, "filePath") : gs(j, "path");
    if (j.is_string()) return j.get<std::string>();
    return {};
  } catch (...) {
    // not JSON -> treat the whole text as a path (trimmed)
    auto a = content.find_first_not_of(" \t\r\n");
    auto b = content.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? std::string() : content.substr(a, b - a + 1);
  }
}

d::Result<d::Body> bodyFromEditor(const std::string &mode, const std::string &content) {
  try {
    if (mode == w::kBodyJson) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Json, content));
    if (mode == w::kBodyText) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Text, content));
    if (mode == w::kBodyXml) return d::Result<d::Body>::ok(d::Body::raw(d::RawSubtype::Xml, content));
    if (mode == w::kBodyForm) {
      std::vector<d::FormField> fields;
      auto j = parseGuarded(content, "[]");
      if (j.is_array())
        for (const auto &e : j)
          if (e.is_object()) fields.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
      return d::Result<d::Body>::ok(d::Body::formUrlEncoded(std::move(fields)));
    }
    if (mode == w::kBodyBinary) {
      std::string fp = binaryFilePathFromEditor(content);
      if (fp.empty()) return d::Result<d::Body>::ok(d::Body::none());
      return d::Body::binary(fp);
    }
    return d::Result<d::Body>::ok(d::Body::none());
  } catch (const std::exception &e) {
    return parseErr<d::Body>(e.what());
  }
}

// ---- gRPC metadata ----
std::string metadataToJson(const d::GrpcMetadata &md) {
  json a = json::array();
  for (const auto &e : md.entries())
    a.push_back({{"key", e.key}, {"value", e.value}, {"enabled", e.enabled ? 1 : 0}});
  return a.dump(2);
}
d::Result<d::GrpcMetadata> jsonToMetadata(const std::string &text) {
  try {
    auto j = parseGuarded(text, "[]");
    if (!j.is_array()) return parseErr<d::GrpcMetadata>("metadata must be a JSON array");
    std::vector<d::MetadataEntry> entries;
    for (const auto &e : j) {
      if (!e.is_object()) continue;
      entries.push_back({gs(e, "key"), gs(e, "value"), gb(e, "enabled", true)});
    }
    return d::GrpcMetadata::create(std::move(entries));
  } catch (const std::exception &e) {
    return parseErr<d::GrpcMetadata>(e.what());
  }
}

// ---- Response headers (display) ----
std::string responseHeadersToJson(const std::vector<d::ResponseHeader> &headers) {
  json a = json::array();
  for (const auto &h : headers) a.push_back({{"key", h.name}, {"value", h.value}});
  return a.dump(2);
}

// ---- Kafka record (display) ----
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
      j["value"] = json::parse(r.value); // embed parsed JSON when possible (SPEC_kafka §2.2)
    } catch (...) {
      j["value"] = r.value; // else raw string
    }
  }
  json hs = json::array();
  for (const auto &h : r.headers)
    if (h.enabled) hs.push_back({{"key", h.key}, {"value", h.value}});
  j["headers"] = hs;
  j["timestampMs"] = r.timestampMs;
  j["size"] = r.size;
  return j.dump(2);
}

// ---- Kafka editor tabs (SPEC_kafka §2/§4) ----
namespace {
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
  // Embed `value` as a REAL nested JSON value (object/array/...), not a JSON-encoded string — the value is
  // always JSON now (no raw/binary mode), so show it the way the user will actually edit it.
  try {
    j["value"] = json::parse(m.value.value);
  } catch (...) {
    j["value"] = m.value.value; // not valid JSON yet (empty draft / mid-edit) -> show verbatim as a string
  }
  j["headers"] = kafkaKvToJson(m.headers);
  return j.dump(2);
}
d::Result<d::KafkaMessage> jsonToKafkaMessage(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    d::KafkaMessage m;
    std::string key = gs(j, "key");
    if (!key.empty()) m.key = d::MessageKey{key};
    if (auto it = j.find("value"); it != j.end()) {
      // A real JSON value (object/array/number/bool/null) -> re-serialize to the text actually sent to
      // Kafka. A JSON string -> use its content directly (back-compat with the old string-encoded form).
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
    c.extra = kafkaKvFromJson<d::KafkaExtra>(j.value("extra", json::array()));
    return d::Result<d::KafkaConsumeConfig>::ok(std::move(c));
  } catch (const std::exception &e) {
    return parseErr<d::KafkaConsumeConfig>(e.what());
  }
}

// ---- Generic JSON text helpers ----
std::string formatJson(const std::string &text, bool pretty) {
  try {
    auto j = json::parse(text);
    return pretty ? j.dump(2) : j.dump();
  } catch (...) {
    return text;
  }
}
std::string jsonEncodeString(const std::string &text) {
  json j = text;
  return j.dump();
}
std::string jsonDecodeString(const std::string &text) {
  try {
    auto j = json::parse(text);
    if (j.is_string()) return j.get<std::string>();
    return text;
  } catch (...) {
    return text;
  }
}

// ---- Config ----
std::string configToJson(const d::RequestConfig &c) {
  json j;
  j["timeout_ms"] = c.timeout.millis();
  j["tls"] = c.tlsEnabledDefault;
  return j.dump(2);
}
d::Result<d::RequestConfig> jsonToConfig(const std::string &text) {
  try {
    auto j = parseGuarded(text, "{}");
    if (!j.is_object()) return parseErr<d::RequestConfig>("config must be a JSON object");
    long long ms = 30000;
    if (auto it = j.find("timeout_ms"); it != j.end() && it->is_number()) ms = it->get<long long>();
    auto t = d::Timeout::fromMillis(ms);
    if (!t) return d::Result<d::RequestConfig>::fail(t.error());
    return d::Result<d::RequestConfig>::ok(d::RequestConfig{t.take(), gb(j, "tls", true)});
  } catch (const std::exception &e) {
    return parseErr<d::RequestConfig>(e.what());
  }
}

// ---- Config (Kafka only — no "tls" key) ----
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
