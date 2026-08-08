#include "infra/transport/kafka/kafka_conf.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

#include "infra/transport/kafka/avro_serde.hpp"

namespace core::infra::kafka_detail {

void consume(const std::string &bootstrap, const d::KafkaSecurity &security, const d::KafkaConsumeSpec &spec,
            d::IResponseSink &sink, const d::ICancellationToken &cancel, std::chrono::milliseconds requestTimeout,
            SchemaRegistryClient &registry) {
  std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
  setConf(conf.get(), "bootstrap.servers", bootstrap);
  applySecurity(conf.get(), security);
  setConf(conf.get(), "group.id", spec.config.group.value());
  setConf(conf.get(), "client.id", spec.config.clientId);
  setConf(conf.get(), "auto.offset.reset",
         spec.config.offsetReset == d::OffsetReset::Earliest ? "earliest" : "latest");
  setConf(conf.get(), "enable.auto.commit", spec.config.autoCommit ? "true" : "false");
  applyExtra(conf.get(), spec.config.extra);

  std::string err;
  std::unique_ptr<RdKafka::KafkaConsumer> consumer(RdKafka::KafkaConsumer::create(conf.get(), err));
  if (!consumer) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Internal, "consumer create failed: " + err, {}}}));
    return;
  }

  // consume() never surfaces a bad bootstrap (it just returns TIMED_OUT forever), so validate connectivity
  // up-front with a bounded metadata() call, sliced so Cancel wins within kCancelSliceMs. ONE deadline
  // bounds the whole session, phase-based: no broker within it = Network failure, Cancel while connecting
  // = Cancelled failure, but deadline/Cancel AFTER the consumer is live = graceful EvClosed (a bounded
  // tail that ends is the feature, not an error).
  const auto deadline = std::chrono::steady_clock::now() + requestTimeout;
  {
    // A metadata slice against a down/starting broker returns TRANSPORT/RESOLVE, not just TIMED_OUT;
    // librdkafka keeps reconnecting underneath, so these stay retryable until the deadline. Anything else
    // (auth, fatal) ends the connect phase at once.
    const auto stillConnecting = [](RdKafka::ErrorCode e) {
      return e == RdKafka::ERR__TIMED_OUT || e == RdKafka::ERR__TRANSPORT ||
             e == RdKafka::ERR__ALL_BROKERS_DOWN || e == RdKafka::ERR__RESOLVE;
    };
    RdKafka::ErrorCode cerr;
    while (true) {
      if (cancel.cancelled()) {
        // Settle the stream FIRST — close() against an unreachable broker can itself block.
        // Cancelled before the consumer ever went live -> EvFailed, not a graceful EvClosed.
        sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Cancelled, "Cancelled", {}}}));
        consumer->close();
        return;
      }
      const long long leftMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
      if (leftMs <= 0) { cerr = RdKafka::ERR__TIMED_OUT; break; }
      RdKafka::Metadata *rawMd = nullptr;
      cerr = consumer->metadata(true, nullptr, &rawMd,
                                static_cast<int>(std::min<long long>(leftMs, kCancelSliceMs)));
      std::unique_ptr<RdKafka::Metadata> md(rawMd); // owns the reply; librdkafka hands us a heap Metadata
      if (!stillConnecting(cerr)) break; // reached a broker (or a real, non-connectivity error)
    }
    if (cerr != RdKafka::ERR_NO_ERROR) {
      // Deadline expired while still connecting -> report Network, not a bare "Local: Timed out".
      MappedErr m = stillConnecting(cerr) ? MappedErr{d::ErrorKind::Network, kBrokerUnreachableMsg}
                                          : mapConsumeErr(cerr);
      sink.emit(d::ResponseEvent(d::EvFailed{{m.kind, m.message, {}}}));
      consumer->close();
      return;
    }
  }

  RdKafka::ErrorCode serr;
  if (spec.config.partition) {
    std::vector<RdKafka::TopicPartition *> parts;
    for (const auto &t : spec.config.topics)
      parts.push_back(RdKafka::TopicPartition::create(t.value(), spec.config.partition->value));
    serr = consumer->assign(parts);
    RdKafka::TopicPartition::destroy(parts);
  } else {
    std::vector<std::string> topics;
    for (const auto &t : spec.config.topics) topics.push_back(t.value());
    serr = consumer->subscribe(topics);
  }
  if (serr != RdKafka::ERR_NO_ERROR) {
    auto m = mapConsumeErr(serr);
    sink.emit(d::ResponseEvent(d::EvFailed{{m.kind, m.message, {}}}));
    consumer->close();
    return;
  }

  int count = 0;
  // Cap each consume() wait at kCancelSliceMs so the token is re-checked promptly; behavior is otherwise
  // identical (consume returns as soon as a record arrives, a TIMED_OUT slice == "no data yet").
  const int pollMs = static_cast<int>(spec.config.pollTimeout.count());
  const int sliceMs = std::min(pollMs, kCancelSliceMs);
  // The session deadline also bounds the live tail: expiring here is the bounded-tail SUCCESS path.
  bool timedOut = false;
  while (!cancel.cancelled()) {
    if (std::chrono::steady_clock::now() >= deadline) { timedOut = true; break; }
    std::unique_ptr<RdKafka::Message> msg(consumer->consume(sliceMs));
    RdKafka::ErrorCode merr = msg->err();
    if (merr == RdKafka::ERR_NO_ERROR) {
      d::KafkaRecord rec;
      rec.topic = msg->topic_name();
      rec.partition = msg->partition();
      rec.offset = msg->offset();
      if (msg->key()) rec.key = *msg->key();
      if (msg->payload() != nullptr && msg->len() > 0) {
        rec.value.assign(static_cast<const char *>(msg->payload()), msg->len());
        // Confluent-Avro auto-detect: magic 0x00 + schema id + configured registry -> decode for display;
        // any failure degrades to verbatim bytes + a note. One blocking fetch per NEW schema id;
        // the negative cache bounds a down registry.
        if (spec.config.schemaRegistry.configured()) {
          if (auto id = avro_serde::extractConfluentSchemaId(rec.value)) {
            auto schema = registry.schemaById(spec.config.schemaRegistry, *id, cancel);
            auto decoded = schema.isOk()
                               ? avro_serde::avroBinaryToJson(schema.value(), rec.value.data() + 5,
                                                              rec.value.size() - 5)
                               : d::Result<std::string>::fail(schema.error());
            if (decoded.isOk()) {
              rec.value = decoded.take();
              rec.valueEncoding = "avro (id " + std::to_string(*id) + ")";
            } else {
              rec.valueEncoding = "avro (id " + std::to_string(*id) +
                                  ", undecoded: " + decoded.error().message + ")";
            }
          }
        }
      } else {
        rec.valueIsNull = true;
      }
      if (auto *hdrs = msg->headers()) {
        // Header values are arbitrary bytes, possibly null — value_string() returns NULL for a null value
        // (UB in std::string) and truncates at embedded '\0'; copy length-based instead.
        const auto all = hdrs->get_all();
        rec.headers.reserve(all.size());
        for (const auto &h : all)
          rec.headers.push_back({h.key(),
                                 h.value() ? std::string(static_cast<const char *>(h.value()), h.value_size())
                                           : std::string(),
                                 true});
      }
      rec.timestampMs = msg->timestamp().timestamp;
      rec.size = msg->len();
      sink.emit(d::ResponseEvent(d::EvKafkaRecord{std::move(rec)}));
      ++count;
    } else if (merr == RdKafka::ERR__PARTITION_EOF || merr == RdKafka::ERR__TIMED_OUT) {
      // end of currently available data / empty poll — not an error
    } else {
      auto m = mapConsumeErr(merr);
      sink.emit(d::ResponseEvent(d::EvFailed{{m.kind, m.message, {}}}));
      consumer->close();
      return;
    }
    if (spec.config.maxMessages && count >= *spec.config.maxMessages) break;
  }

  bool cancelled = cancel.cancelled();
  // Settle the stream BEFORE close(): leave-group + final commit can block for seconds. All three exits
  // (Cancel mid-consume, session deadline, maxMessages) are graceful EvClosed — the consumer WAS live.
  sink.emit(d::ResponseEvent(
      d::EvClosed{std::nullopt, cancelled ? "cancelled" : timedOut ? "timeout" : "stopped"}));
  consumer->close(); // commits final offsets if auto-commit is on
}

} // namespace core::infra::kafka_detail
