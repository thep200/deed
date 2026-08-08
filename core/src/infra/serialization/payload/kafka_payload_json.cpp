#include "infra/serialization/payload/payload_json.hpp"

#include <optional>
#include <variant>

#include "core/infra/serialization/field_json.hpp"

namespace core::infra::payload {

namespace {

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

json securityToJson(const d::KafkaSecurity &s) {
  json j;
  s.match([&](auto &&v) {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, d::KafkaPlaintext>) j["type"] = "plaintext";
  });
  return j;
}

d::Result<d::KafkaSecurity> securityFromJson(const json &j) {
  std::string type = j.is_object() ? gs(j, "type", "plaintext") : "plaintext";
  if (type != "plaintext")
    return d::Result<d::KafkaSecurity>::fail(
        {d::ErrorCode::Unsupported, "unsupported kafka security type: " + type, "kafka.security.type"});
  return d::Result<d::KafkaSecurity>::ok(d::KafkaSecurity::plaintext());
}

json producerToJson(const d::KafkaProduceSpec &p) {
  return json{{"config", serialTo(core::serial::kafkaProduceConfigToJson(p.config))},
              {"message", serialTo(core::serial::kafkaMessageToJson(p.message))}};
}

d::Result<d::KafkaProduceSpec> producerFromJson(const json &b) {
  auto cfg = serialFrom<d::KafkaProduceConfig>(b, "config", core::serial::jsonToKafkaProduceConfig, "{}");
  if (!cfg) return d::Result<d::KafkaProduceSpec>::fail(cfg.error());
  auto msg = serialFrom<d::KafkaMessage>(b, "message", core::serial::jsonToKafkaMessage, "{}");
  if (!msg) return d::Result<d::KafkaProduceSpec>::fail(msg.error());
  return d::Result<d::KafkaProduceSpec>::ok(d::KafkaProduceSpec{cfg.take(), msg.take()});
}

json consumerToJson(const d::KafkaConsumeSpec &spec) {
  return json{{"config", serialTo(core::serial::kafkaConsumeConfigToJson(spec.config))}};
}

d::Result<d::KafkaConsumeSpec> consumerFromJson(const json &outer) {
  auto cfg = serialFrom<d::KafkaConsumeConfig>(outer, "config", core::serial::jsonToKafkaConsumeConfig, "{}");
  if (!cfg) return d::Result<d::KafkaConsumeSpec>::fail(cfg.error());
  return d::Result<d::KafkaConsumeSpec>::ok(d::KafkaConsumeSpec{cfg.take()});
}

} // namespace

json toJson(const d::KafkaRequest &k) {
  json j{{"brokers", k.brokers().toBootstrapServers()},
         {"clientKind", k.kind() == d::KafkaClientKind::Producer ? "producer" : "consumer"},
         {"security", securityToJson(k.security())},
         {"producer", nullptr},
         {"consumer", nullptr}};
  auto fillSlot = overloaded{
      [&](const d::KafkaProduceSpec &p) { j["producer"] = producerToJson(p); },
      [&](const d::KafkaConsumeSpec &c) { j["consumer"] = consumerToJson(c); },
  };
  k.match(fillSlot);
  // Both sides persist: the inactive draft fills the other slot so toggling Producer/Consumer doesn't lose typed input.
  if (k.inactiveDraft()) std::visit(fillSlot, *k.inactiveDraft());
  return j;
}

d::Result<Payload> parse(d::TypeTag<d::KafkaRequest>, const json &b) {
  auto brokers = d::BrokerList::parse(gs(b, "brokers"));
  if (!brokers) return d::Result<Payload>::fail(brokers.error());
  auto security = securityFromJson(b.value("security", json::object()));
  if (!security) return d::Result<Payload>::fail(security.error());

  std::string kind = gs(b, "clientKind", "producer");
  d::Result<d::KafkaRequest::Mode> mode = [&]() -> d::Result<d::KafkaRequest::Mode> {
    if (kind == "consumer") {
      auto c = consumerFromJson(b.value("consumer", json::object()));
      if (!c) return d::Result<d::KafkaRequest::Mode>::fail(c.error());
      return d::Result<d::KafkaRequest::Mode>::ok(d::KafkaRequest::Mode{c.take()});
    }
    auto p = producerFromJson(b.value("producer", json::object()));
    if (!p) return d::Result<d::KafkaRequest::Mode>::fail(p.error());
    return d::Result<d::KafkaRequest::Mode>::ok(d::KafkaRequest::Mode{p.take()});
  }();
  if (!mode) return d::Result<Payload>::fail(mode.error());

  // Non-null other slot = the preserved inactive draft. Lenient on purpose: a corrupt draft drops
  // silently rather than bricking the request (only the active side is authoritative).
  std::optional<d::KafkaRequest::Mode> draft;
  if (kind == "consumer") {
    if (const auto it = b.find("producer"); it != b.end() && !it->is_null()) {
      if (auto p = producerFromJson(*it)) draft = d::KafkaRequest::Mode{p.take()};
    }
  } else {
    if (const auto it = b.find("consumer"); it != b.end() && !it->is_null()) {
      if (auto c = consumerFromJson(*it)) draft = d::KafkaRequest::Mode{c.take()};
    }
  }

  auto r = d::KafkaRequest::create(brokers.value(), security.value(), mode.value(), std::move(draft));
  // Same leniency for a draft that parses but fails create()'s invariants: retry without it.
  if (!r) r = d::KafkaRequest::create(brokers.take(), security.take(), mode.take());
  if (!r) return d::Result<Payload>::fail(r.error());
  return d::Result<Payload>::ok(Payload{r.take()});
}

} // namespace core::infra::payload
