#include "infra/transport/kafka/kafka_conf.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <librdkafka/rdkafkacpp.h>

#include "infra/transport/kafka/avro_serde.hpp"

namespace core::infra::kafka_detail {

namespace {

class DrCb final : public RdKafka::DeliveryReportCb {
public:
  void dr_cb(RdKafka::Message &message) override {
    got = true;
    err = message.err();
    partition = message.partition();
    offset = message.offset();
    size = message.len();
    latencyUs = message.latency();
  }
  bool got = false;
  RdKafka::ErrorCode err = RdKafka::ERR_NO_ERROR;
  int32_t partition = 0;
  int64_t offset = 0;
  size_t size = 0;
  int64_t latencyUs = 0;
};

} // namespace

void produce(const std::string &bootstrap, const d::KafkaSecurity &security, const d::KafkaProduceSpec &spec,
            d::IResponseSink &sink, const d::ICancellationToken &cancel, std::chrono::milliseconds requestTimeout,
            SchemaRegistryClient &registry) {
  // Avro: serialize the JSON editor value against the LATEST schema of "<topic>-value", Confluent-framed.
  // Fail fast, before any broker objects; ErrorKind has no Validation -> Protocol.
  std::string wireValue;
  const std::string *valuePtr = &spec.message.value.value;
  if (spec.config.valueFormat == d::KafkaValueFormat::Avro) {
    if (!spec.config.schemaRegistry.configured()) {
      sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Protocol,
          "Avro value format needs \"schemaRegistry\": {\"url\": ...} in the Kafka tab", {}}}));
      return;
    }
    if (spec.message.value.value.find_first_not_of(" \t\r\n") == std::string::npos) {
      sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Protocol,
          "message value is empty — Avro needs a JSON value matching the subject's schema", {}}}));
      return;
    }
    const std::string subject = spec.config.topic.value() + "-value";
    auto latest = registry.latestForSubject(spec.config.schemaRegistry, subject, cancel);
    if (!latest.isOk()) {
      sink.emit(d::ResponseEvent(
          d::EvFailed{{d::ErrorKind::Protocol, subject + ": " + latest.error().message, {}}}));
      return;
    }
    auto bin = avro_serde::jsonToAvroBinary(latest.value().schemaJson, spec.message.value.value);
    if (!bin.isOk()) {
      sink.emit(d::ResponseEvent(d::EvFailed{
          {d::ErrorKind::Parse, "Avro serialization failed: " + bin.error().message, {}}}));
      return;
    }
    wireValue = avro_serde::wrapConfluent(latest.value().id, bin.take());
    valuePtr = &wireValue;
  }

  std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  setConf(conf.get(), "bootstrap.servers", bootstrap);
  applySecurity(conf.get(), security);
  setConf(conf.get(), "client.id", spec.config.clientId);
  setConf(conf.get(), "acks", acksProp(spec.config.acks));
  setConf(conf.get(), "compression.type", compressionProp(spec.config.compression));
  setConf(conf.get(), "message.timeout.ms", std::to_string(spec.config.messageTimeout.count()));
  setConf(conf.get(), "linger.ms", std::to_string(spec.config.linger.count()));
  setConf(conf.get(), "retries", std::to_string(spec.config.retries));
  setConf(conf.get(), "enable.idempotence", spec.config.idempotence ? "true" : "false");
  applyExtra(conf.get(), spec.config.extra);

  DrCb drCb;
  std::string err;
  conf->set("dr_cb", &drCb, err);

  std::unique_ptr<RdKafka::Producer> producer(RdKafka::Producer::create(conf.get(), err));
  if (!producer) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Internal, "producer create failed: " + err, {}}}));
    return;
  }

  RdKafka::Headers *headers = RdKafka::Headers::create();
  for (const auto &h : spec.message.headers)
    if (h.enabled) headers->add(h.key, h.value);

  const void *keyPtr = nullptr;
  size_t keyLen = 0;
  if (spec.message.key) {
    keyPtr = spec.message.key->value.data();
    keyLen = spec.message.key->value.size();
  }
  void *payload = const_cast<char *>(valuePtr->data()); // RK_MSG_COPY below -> wireValue lifetime is fine
  size_t payloadLen = valuePtr->size();
  int32_t partition = spec.config.partition.value == d::KafkaPartition::kAuto
                          ? RdKafka::Topic::PARTITION_UA
                          : spec.config.partition.value;

  RdKafka::ErrorCode perr =
      producer->produce(spec.config.topic.value(), partition, RdKafka::Producer::RK_MSG_COPY, payload,
                        payloadLen, keyPtr, keyLen, 0, headers, nullptr);
  if (perr != RdKafka::ERR_NO_ERROR) {
    delete headers; // ownership transfers to the producer only on success
    auto m = mapProduceErr(perr);
    sink.emit(d::ResponseEvent(d::EvFailed{{m.kind, m.message, {}}}));
    return;
  }

  // flush() as ONE blocking call cannot be interrupted (Cancel + the shared Config timeout would be
  // ignored) — flush in short slices, checking the token and an outer deadline (min of messageTimeoutMs
  // and the Config timeout) between slices.
  const auto effectiveTimeout = std::min(spec.config.messageTimeout, requestTimeout);
  const auto deadline = std::chrono::steady_clock::now() + effectiveTimeout;
  constexpr int kFlushSliceMs = 100;
  constexpr int kPurgeDrainMs = 1000; // post-purge flush: drain the purge-completion delivery report
  bool cancelled = false;
  bool timedOut = false;
  while (true) {
    RdKafka::ErrorCode ferr = producer->flush(kFlushSliceMs);
    if (ferr != RdKafka::ERR__TIMED_OUT) break; // flushed (drCb fired) or a real error — stop slicing
    if (cancel.cancelled()) { cancelled = true; break; }
    if (std::chrono::steady_clock::now() >= deadline) { timedOut = true; break; }
  }
  if (cancelled) {
    // Abort queued + in-flight so flush() returns promptly instead of waiting out message.timeout.ms.
    producer->purge(RdKafka::Producer::PURGE_QUEUE | RdKafka::Producer::PURGE_INFLIGHT);
    producer->flush(kPurgeDrainMs);
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Cancelled, "Cancelled", {}}}));
    return;
  }
  if (timedOut) {
    // Drain so the producer destroys cleanly, but report a plain Timeout — the purge is just how we
    // unblock the wait, not the cause.
    if (!drCb.got) {
      producer->purge(RdKafka::Producer::PURGE_QUEUE | RdKafka::Producer::PURGE_INFLIGHT);
      producer->flush(kPurgeDrainMs);
    }
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Timeout, kDeliveryTimedOutMsg, {}}}));
    return;
  }

  if (!drCb.got) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Timeout, kDeliveryTimedOutMsg, {}}}));
    return;
  }
  if (drCb.err != RdKafka::ERR_NO_ERROR) {
    auto m = mapProduceErr(drCb.err);
    sink.emit(d::ResponseEvent(d::EvFailed{{m.kind, m.message, {}}}));
    return;
  }

  nlohmann::json summary{{"partition", drCb.partition},
                        {"offset", drCb.offset},
                        {"size", drCb.size},
                        {"latencyMs", drCb.latencyUs / 1000}};
  d::ApiResponse resp;
  resp.statusCode = 0; // success sentinel (same convention as gRPC OK, native_grpc_sender.cpp)
  resp.body = summary.dump(2);
  resp.elapsed = std::chrono::milliseconds(drCb.latencyUs / 1000);
  sink.emit(d::ResponseEvent(d::EvCompleted{std::move(resp)}));
}

} // namespace core::infra::kafka_detail
