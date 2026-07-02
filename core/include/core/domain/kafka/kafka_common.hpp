// core/domain/kafka/kafka_common.hpp — shared value objects for both Kafka client kinds (SPEC_kafka §3).
// Broker list (thanh URL), topic, partition, header/extra passthrough rows, security (Plaintext-only today).
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/domain/common/result.hpp"

namespace core::domain {

struct Broker {
  std::string host;
  std::uint16_t port = 0;
  bool operator==(const Broker &o) const { return host == o.host && port == o.port; }
};

// Invariant: non-empty, every "host:port" entry must parse (host non-empty, port a valid uint16).
class BrokerList {
public:
  static Result<BrokerList> parse(std::string csv) {
    std::vector<Broker> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
      // trim
      auto b = item.find_first_not_of(" \t");
      auto e = item.find_last_not_of(" \t");
      if (b == std::string::npos) continue;
      item = item.substr(b, e - b + 1);
      if (item.empty()) continue;
      auto sep = item.rfind(':');
      if (sep == std::string::npos || sep == 0 || sep == item.size() - 1)
        return Result<BrokerList>::fail({ErrorCode::Validation, "invalid broker \"" + item + "\" (want host:port)",
                                         "kafka.brokers"});
      std::string host = item.substr(0, sep);
      std::string portStr = item.substr(sep + 1);
      int port = 0;
      for (unsigned char c : portStr) {
        if (c < '0' || c > '9')
          return Result<BrokerList>::fail(
              {ErrorCode::Validation, "invalid broker port \"" + portStr + "\"", "kafka.brokers"});
        port = port * 10 + (c - '0');
      }
      if (portStr.empty() || port <= 0 || port > 65535)
        return Result<BrokerList>::fail(
            {ErrorCode::Validation, "invalid broker port \"" + portStr + "\"", "kafka.brokers"});
      out.push_back(Broker{std::move(host), static_cast<std::uint16_t>(port)});
    }
    if (out.empty())
      return Result<BrokerList>::fail({ErrorCode::Validation, "brokers must not be empty", "kafka.brokers"});
    return Result<BrokerList>::ok(BrokerList(std::move(out)));
  }

  std::string toBootstrapServers() const {
    std::string out;
    for (std::size_t i = 0; i < brokers_.size(); ++i) {
      if (i) out += ",";
      out += brokers_[i].host + ":" + std::to_string(brokers_[i].port);
    }
    return out;
  }

  const std::vector<Broker> &brokers() const noexcept { return brokers_; }

  bool operator==(const BrokerList &o) const { return brokers_ == o.brokers_; }
  bool operator!=(const BrokerList &o) const { return !(*this == o); }

private:
  explicit BrokerList(std::vector<Broker> b) : brokers_(std::move(b)) {}
  std::vector<Broker> brokers_;
};

// Invariant: non-empty.
class KafkaTopic {
public:
  static Result<KafkaTopic> create(std::string name) {
    if (name.empty())
      return Result<KafkaTopic>::fail({ErrorCode::Validation, "topic must not be empty", "kafka.topic"});
    return Result<KafkaTopic>::ok(KafkaTopic(std::move(name)));
  }
  const std::string &value() const noexcept { return name_; }
  bool operator==(const KafkaTopic &o) const { return name_ == o.name_; }
  bool operator!=(const KafkaTopic &o) const { return !(*this == o); }

private:
  explicit KafkaTopic(std::string n) : name_(std::move(n)) {}
  std::string name_;
};

// -1 (kAuto) = auto-assign (producer) / all partitions via subscribe (consumer); >=0 = a specific partition.
struct KafkaPartition {
  static constexpr int kAuto = -1;
  int value = kAuto;
  bool operator==(const KafkaPartition &o) const { return value == o.value; }
};

// Passthrough rows — no validation (librdkafka accepts arbitrary bytes for message headers; extra[] is a
// raw property-name/value escape hatch), unlike the HTTP `Header` VO which enforces RFC 7230 tokens.
struct KafkaHeader {
  std::string key;
  std::string value;
  bool enabled = true;
  bool operator==(const KafkaHeader &o) const {
    return key == o.key && value == o.value && enabled == o.enabled;
  }
};
struct KafkaExtra {
  std::string key;
  std::string value;
  bool enabled = true;
  bool operator==(const KafkaExtra &o) const {
    return key == o.key && value == o.value && enabled == o.enabled;
  }
};

// Security — sum type; only Plaintext today (SSL/SASL are a seam, spec §8).
struct KafkaPlaintext {
  bool operator==(const KafkaPlaintext &) const { return true; }
};
class KafkaSecurity {
public:
  using Variant = std::variant<KafkaPlaintext>;

  static KafkaSecurity plaintext() { return KafkaSecurity(KafkaPlaintext{}); }

  template <class V> decltype(auto) match(V &&v) const { return std::visit(std::forward<V>(v), data_); }

  bool operator==(const KafkaSecurity &o) const { return data_ == o.data_; }
  bool operator!=(const KafkaSecurity &o) const { return !(*this == o); }

private:
  explicit KafkaSecurity(Variant v) : data_(std::move(v)) {}
  Variant data_;
};

} // namespace core::domain
