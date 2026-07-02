// core/domain/kafka/kafka_consume.hpp — consumer config + record VOs (SPEC_kafka §3, tab Config / pane phải).
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/kafka/kafka_common.hpp"

namespace core::domain {

namespace detail {
// Pure-STL random suffix (no external lib) for the auto-generated "deed-tail-<id>" group name.
inline std::string randomLowerAlnum(std::size_t n) {
  static std::mutex mu;
  static std::mt19937_64 rng([] {
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  }());
  static const char *alphabet = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::lock_guard<std::mutex> lk(mu);
  std::string out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) out += alphabet[rng() % 36];
  return out;
}
} // namespace detail

// Invariant: none on the string itself — an empty id auto-generates "deed-tail-<uuid>" (spec §3).
class ConsumerGroup {
public:
  static Result<ConsumerGroup> create(std::string id) {
    if (id.empty()) id = "deed-tail-" + detail::randomLowerAlnum(8);
    return Result<ConsumerGroup>::ok(ConsumerGroup(std::move(id)));
  }
  const std::string &value() const noexcept { return id_; }
  bool operator==(const ConsumerGroup &o) const { return id_ == o.id_; }
  bool operator!=(const ConsumerGroup &o) const { return !(*this == o); }

private:
  explicit ConsumerGroup(std::string id) : id_(std::move(id)) {}
  std::string id_;
};

enum class OffsetReset { Earliest, Latest }; // -> auto.offset.reset

// New-request consumer defaults (SPEC_kafka §3). Pure domain — seed a fresh request's Config tab;
// per-request, user-edited in the UI (not app-global .env tunables).
inline constexpr std::chrono::milliseconds kDefaultPollTimeout{500};
inline constexpr const char *kDefaultConsumerClientId = "deed";

// Invariants (topics non-empty, partition -1/nullopt/>=0, maxMessages>0 if set, pollTimeout>0) are checked
// by KafkaRequest::create (kafka_request.hpp) — this struct is a plain aggregate, per the spec's pseudocode.
struct KafkaConsumeConfig {
  std::vector<KafkaTopic> topics;
  std::optional<KafkaPartition> partition; // present => assign() one partition; absent => subscribe()
  ConsumerGroup group;
  OffsetReset offsetReset = OffsetReset::Latest;
  bool autoCommit = true;
  std::optional<int> maxMessages; // nullopt = unbounded; >0 = stop after N
  std::chrono::milliseconds pollTimeout{kDefaultPollTimeout};
  std::string clientId = kDefaultConsumerClientId;
  std::vector<KafkaExtra> extra;

  bool operator==(const KafkaConsumeConfig &o) const {
    return topics == o.topics && partition == o.partition && group == o.group &&
           offsetReset == o.offsetReset && autoCommit == o.autoCommit && maxMessages == o.maxMessages &&
           pollTimeout == o.pollTimeout && clientId == o.clientId && extra == o.extra;
  }
  bool operator!=(const KafkaConsumeConfig &o) const { return !(*this == o); }
};

// One consumed record — OUTPUT THUẦN (spec §3/§6): verbatim bytes, no domain processing between receive and
// display. Pretty-print/hex display is a UI-only concern built from `value`, never mutating it.
struct KafkaRecord {
  std::string topic;
  int partition = 0;
  std::int64_t offset = 0;
  std::optional<std::string> key; // nullopt if the key was null
  std::string value;              // verbatim bytes; tombstone => empty + valueIsNull
  bool valueIsNull = false;
  std::vector<KafkaHeader> headers;
  std::int64_t timestampMs = 0;
  std::size_t size = 0;

  bool operator==(const KafkaRecord &o) const {
    return topic == o.topic && partition == o.partition && offset == o.offset && key == o.key &&
           value == o.value && valueIsNull == o.valueIsNull && headers == o.headers &&
           timestampMs == o.timestampMs && size == o.size;
  }
};

struct KafkaConsumeSpec {
  KafkaConsumeConfig config;
  bool operator==(const KafkaConsumeSpec &o) const { return config == o.config; }
};

} // namespace core::domain
