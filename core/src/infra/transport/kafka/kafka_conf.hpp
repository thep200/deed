#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

#include "infra/transport/kafka/schema_registry_client.hpp"
#include "infra/transport/typed_sender.hpp"

namespace core::infra::kafka_detail {
namespace d = core::domain;

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

void setConf(RdKafka::Conf *conf, const std::string &key, const std::string &val);
void applyExtra(RdKafka::Conf *conf, const std::vector<d::KafkaExtra> &extra);
void applySecurity(RdKafka::Conf *conf, const d::KafkaSecurity &security);
const char *acksProp(d::Acks a);
const char *compressionProp(d::Compression c);

// Cancel-latency ceiling: no librdkafka wait (connect probe / poll / flush) may block longer than this
// between checks of the cancellation token.
constexpr int kCancelSliceMs = 200;

constexpr const char *kDeliveryTimedOutMsg = "Delivery timed out — is the broker running?";
constexpr const char *kBrokerUnreachableMsg =
    "Could not connect to the broker — check host:port and whether Kafka has started.";

struct MappedErr {
  d::ErrorKind kind;
  std::string message;
};

MappedErr mapProduceErr(RdKafka::ErrorCode err);
MappedErr mapConsumeErr(RdKafka::ErrorCode err);

void produce(const std::string &bootstrap, const d::KafkaSecurity &security, const d::KafkaProduceSpec &spec,
            d::IResponseSink &sink, const d::ICancellationToken &cancel, std::chrono::milliseconds requestTimeout,
            SchemaRegistryClient &registry);
void consume(const std::string &bootstrap, const d::KafkaSecurity &security, const d::KafkaConsumeSpec &spec,
            d::IResponseSink &sink, const d::ICancellationToken &cancel, std::chrono::milliseconds requestTimeout,
            SchemaRegistryClient &registry);

} // namespace core::infra::kafka_detail
