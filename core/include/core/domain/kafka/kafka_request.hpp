// core/domain/kafka/kafka_request.hpp — KafkaRequest aggregate payload (SPEC_kafka §3). ONE RequestType,
// two client kinds chosen by which Mode alternative is held (mirrors WebSocketRequest's shape).
#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "core/domain/common/result.hpp"
#include "core/domain/kafka/kafka_common.hpp"
#include "core/domain/kafka/kafka_consume.hpp"
#include "core/domain/kafka/kafka_produce.hpp"

namespace core::domain {

enum class KafkaClientKind { Producer, Consumer };

class KafkaRequest {
public:
  using Mode = std::variant<KafkaProduceSpec, KafkaConsumeSpec>; // = client-kind selector

  // Re-validates invariants the leaf VOs can't check alone (each side sees only its own struct, this sees
  // the whole Mode): producer partition -1|>=0; consumer topics non-empty, partition -1|nullopt|>=0,
  // maxMessages>0 if set, pollTimeout>0 (SPEC_kafka §3 bất biến).
  static Result<KafkaRequest> create(BrokerList brokers, KafkaSecurity security, Mode mode) {
    bool ok = true;
    std::string msg, field;
    std::visit(
        [&](auto &&m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, KafkaProduceSpec>) {
            int p = m.config.partition.value;
            if (p != KafkaPartition::kAuto && p < 0) {
              ok = false;
              msg = "partition must be -1 (auto) or >= 0";
              field = "kafka.producer.config.partition";
            }
          } else if constexpr (std::is_same_v<T, KafkaConsumeSpec>) {
            if (m.config.topics.empty()) {
              ok = false;
              msg = "topics must not be empty";
              field = "kafka.consumer.config.topics";
            } else if (m.config.partition && m.config.partition->value != KafkaPartition::kAuto &&
                       m.config.partition->value < 0) {
              ok = false;
              msg = "partition must be -1 (all) or >= 0";
              field = "kafka.consumer.config.partition";
            } else if (m.config.maxMessages && *m.config.maxMessages <= 0) {
              ok = false;
              msg = "maxMessages must be > 0 when set";
              field = "kafka.consumer.config.maxMessages";
            } else if (m.config.pollTimeout.count() <= 0) {
              ok = false;
              msg = "pollTimeout must be > 0";
              field = "kafka.consumer.config.pollTimeout";
            }
          }
        },
        mode);
    if (!ok) return Result<KafkaRequest>::fail({ErrorCode::Validation, msg, field});
    return Result<KafkaRequest>::ok(KafkaRequest(std::move(brokers), std::move(security), std::move(mode)));
  }

  KafkaClientKind kind() const noexcept {
    return std::holds_alternative<KafkaProduceSpec>(mode_) ? KafkaClientKind::Producer
                                                            : KafkaClientKind::Consumer;
  }
  const BrokerList &brokers() const noexcept { return brokers_; }
  const KafkaSecurity &security() const noexcept { return security_; }
  const Mode &mode() const noexcept { return mode_; }

  template <class V> decltype(auto) match(V &&v) const { return std::visit(std::forward<V>(v), mode_); }

  bool operator==(const KafkaRequest &o) const {
    return brokers_ == o.brokers_ && security_ == o.security_ && mode_ == o.mode_;
  }
  bool operator!=(const KafkaRequest &o) const { return !(*this == o); }

private:
  KafkaRequest(BrokerList brokers, KafkaSecurity security, Mode mode)
      : brokers_(std::move(brokers)), security_(std::move(security)), mode_(std::move(mode)) {}

  BrokerList brokers_;
  KafkaSecurity security_;
  Mode mode_;
};

} // namespace core::domain
