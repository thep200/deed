#pragma once

#include <optional>
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

  // Re-validates invariants the leaf VOs can't check alone (this sees the whole Mode).
  // inactiveDraft preserves the OTHER client-kind's last state across toggle/persistence; must hold the opposite alternative.
  static Result<KafkaRequest> create(BrokerList brokers, KafkaSecurity security, Mode mode,
                                     std::optional<Mode> inactiveDraft = std::nullopt) {
    if (auto err = validateMode(mode)) return Result<KafkaRequest>::fail(*err);
    if (inactiveDraft) {
      if (inactiveDraft->index() == mode.index())
        return Result<KafkaRequest>::fail({ErrorCode::Validation,
                                           "inactiveDraft must hold the other client kind",
                                           "kafka.inactiveDraft"});
      if (auto err = validateMode(*inactiveDraft)) return Result<KafkaRequest>::fail(*err);
    }
    return Result<KafkaRequest>::ok(
        KafkaRequest(std::move(brokers), std::move(security), std::move(mode), std::move(inactiveDraft)));
  }

  KafkaClientKind kind() const noexcept {
    return std::holds_alternative<KafkaProduceSpec>(mode_) ? KafkaClientKind::Producer
                                                            : KafkaClientKind::Consumer;
  }
  const BrokerList &brokers() const noexcept { return brokers_; }
  const KafkaSecurity &security() const noexcept { return security_; }
  const Mode &mode() const noexcept { return mode_; }
  const std::optional<Mode> &inactiveDraft() const noexcept { return inactiveDraft_; }

  template <class V> decltype(auto) match(V &&v) const { return std::visit(std::forward<V>(v), mode_); }

  bool operator==(const KafkaRequest &o) const {
    return brokers_ == o.brokers_ && security_ == o.security_ && mode_ == o.mode_ &&
           inactiveDraft_ == o.inactiveDraft_;
  }
  bool operator!=(const KafkaRequest &o) const { return !(*this == o); }

private:
  static std::optional<Error> validateMode(const Mode &mode) {
    std::optional<Error> err;
    std::visit(
        [&](auto &&m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, KafkaProduceSpec>) {
            int p = m.config.partition.value;
            if (p != KafkaPartition::kAuto && p < 0)
              err = Error{ErrorCode::Validation, "partition must be -1 (auto) or >= 0",
                          "kafka.producer.config.partition"};
          } else if constexpr (std::is_same_v<T, KafkaConsumeSpec>) {
            if (m.config.topics.empty())
              err = Error{ErrorCode::Validation, "topics must not be empty", "kafka.consumer.config.topics"};
            else if (m.config.partition && m.config.partition->value != KafkaPartition::kAuto &&
                     m.config.partition->value < 0)
              err = Error{ErrorCode::Validation, "partition must be -1 (all) or >= 0",
                          "kafka.consumer.config.partition"};
            else if (m.config.maxMessages && *m.config.maxMessages <= 0)
              err = Error{ErrorCode::Validation, "maxMessages must be > 0 when set",
                          "kafka.consumer.config.maxMessages"};
            else if (m.config.pollTimeout.count() <= 0)
              err = Error{ErrorCode::Validation, "pollTimeout must be > 0",
                          "kafka.consumer.config.pollTimeout"};
          }
        },
        mode);
    return err;
  }

  KafkaRequest(BrokerList brokers, KafkaSecurity security, Mode mode, std::optional<Mode> inactiveDraft)
      : brokers_(std::move(brokers)), security_(std::move(security)), mode_(std::move(mode)),
        inactiveDraft_(std::move(inactiveDraft)) {}

  BrokerList brokers_;
  KafkaSecurity security_;
  Mode mode_;
  std::optional<Mode> inactiveDraft_; // the OTHER kind's preserved draft (see create())
};

} // namespace core::domain
