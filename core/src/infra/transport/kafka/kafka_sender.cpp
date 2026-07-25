// kafka_sender.cpp — Kafka producer+consumer over librdkafka (SPEC_kafka §6/§7). ONLY file allowed to
// include <rdkafkacpp.h> (layering_gate enforces this — confined to src/infra/transport/kafka).
#include "infra/transport/kafka/kafka_sender.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp> // producer delivery summary (body text) — infra/transport may use it (not domain)
#include <librdkafka/rdkafkacpp.h>

#include "infra/transport/kafka/avro_serde.hpp"

namespace core::infra {
namespace d = core::domain;

namespace {

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// ---- Conf helpers ----
void setConf(RdKafka::Conf *conf, const std::string &key, const std::string &val) {
  std::string err;
  conf->set(key, val, err); // best-effort: a bad extra[] property is the user's problem, not fatal here
}
void applyExtra(RdKafka::Conf *conf, const std::vector<d::KafkaExtra> &extra) {
  for (const auto &e : extra)
    if (e.enabled) setConf(conf, e.key, e.value);
}
// PLAINTEXT is rdkafka's default (no property to set); seam for SSL/SASL alternatives (SPEC_kafka §8).
void applySecurity(RdKafka::Conf *conf, const d::KafkaSecurity &security) {
  security.match(overloaded{[&](const d::KafkaPlaintext &) { (void)conf; }});
}

const char *acksProp(d::Acks a) {
  switch (a) {
  case d::Acks::None: return "0";
  case d::Acks::Leader: return "1";
  case d::Acks::All: return "all";
  }
  return "all";
}
const char *compressionProp(d::Compression c) {
  switch (c) {
  case d::Compression::None: return "none";
  case d::Compression::Gzip: return "gzip";
  case d::Compression::Snappy: return "snappy";
  case d::Compression::Lz4: return "lz4";
  case d::Compression::Zstd: return "zstd";
  }
  return "none";
}

// Cancel-latency ceiling: no librdkafka wait (connect probe / poll) may block longer than this between
// checks of the cancellation token — the Cancel button is the highest-priority signal (SPEC_kafka §5).
constexpr int kCancelSliceMs = 200;

// ---- Error mapping (SPEC_kafka §7) ----
constexpr const char *kDeliveryTimedOutMsg = "Delivery timed out — is the broker running?";
constexpr const char *kBrokerUnreachableMsg =
    "Could not connect to the broker — check host:port and whether Kafka has started.";

struct MappedErr {
  d::ErrorKind kind;
  std::string message;
};

MappedErr mapProduceErr(RdKafka::ErrorCode err) {
  switch (err) {
  case RdKafka::ERR__MSG_TIMED_OUT:
    return {d::ErrorKind::Timeout, kDeliveryTimedOutMsg};
  case RdKafka::ERR__TRANSPORT:
  case RdKafka::ERR__ALL_BROKERS_DOWN:
  case RdKafka::ERR__RESOLVE:
    return {d::ErrorKind::Network, kBrokerUnreachableMsg};
  case RdKafka::ERR_UNKNOWN_TOPIC_OR_PART:
    return {d::ErrorKind::Protocol, "Topic does not exist and auto-create is off — create it first."};
  case RdKafka::ERR_LEADER_NOT_AVAILABLE:
  case RdKafka::ERR_NOT_LEADER_FOR_PARTITION:
    return {d::ErrorKind::Protocol, "Partition leader is not ready yet — try again shortly."};
  case RdKafka::ERR_MSG_SIZE_TOO_LARGE:
    return {d::ErrorKind::Protocol, "Message exceeds the broker/topic's message.max.bytes."};
  default: return {d::ErrorKind::Protocol, RdKafka::err2str(err)};
  }
}
MappedErr mapConsumeErr(RdKafka::ErrorCode err) {
  switch (err) {
  case RdKafka::ERR__TRANSPORT:
  case RdKafka::ERR__ALL_BROKERS_DOWN:
  case RdKafka::ERR__RESOLVE:
    return {d::ErrorKind::Network, kBrokerUnreachableMsg};
  case RdKafka::ERR_UNKNOWN_TOPIC_OR_PART:
    return {d::ErrorKind::Protocol, "Topic does not exist — create it first or enable auto-create."};
  case RdKafka::ERR_GROUP_AUTHORIZATION_FAILED:
  case RdKafka::ERR_TOPIC_AUTHORIZATION_FAILED:
    return {d::ErrorKind::Protocol,
            "Insufficient group/topic permissions — check ACLs (or leave the group blank to tail anonymously)."};
  case RdKafka::ERR__FATAL:
    return {d::ErrorKind::Internal, "Consumer hit a fatal error — check the logs and try Start again."};
  default: return {d::ErrorKind::Protocol, RdKafka::err2str(err)};
  }
}

// ---- Producer (unary-like, spec §5/§6) ----
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

void produce(const std::string &bootstrap, const d::KafkaSecurity &security, const d::KafkaProduceSpec &spec,
            d::IResponseSink &sink, const d::ICancellationToken &cancel, std::chrono::milliseconds requestTimeout,
            SchemaRegistryClient &registry) {
  // Avro value format (SPEC_kafka Avro v1): serialize the (JSON) editor value against the LATEST
  // registered schema of `<topic>-value`, wrapped in the Confluent framing. Fail fast — before any
  // broker objects — with a message that names the fix. ErrorKind has no Validation -> Protocol.
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

  // BUG FIX: producer->flush(bigTimeout) as ONE blocking call had no way to be interrupted — a bad/fake
  // broker meant Cancel did nothing (KafkaSender never overrides close(), so SendRequestSaga::cancel()'s
  // only mechanism was a no-op) and the shared per-request Config-tab timeout (resolved.config().timeout,
  // what every other sender honors) was silently ignored — only the Kafka tab's own messageTimeoutMs
  // (default 30s) applied. Flush in short slices instead, checking cancellation and an outer deadline —
  // the smaller of messageTimeoutMs and the shared Config timeout — between slices (SPEC_kafka §5:
  // "Cancel: token -> rd_kafka_purge(...) -> kết thúc", previously unimplemented).
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
    // Abort queued + in-flight messages so flush() below returns promptly instead of waiting out
    // whatever's left of message.timeout.ms.
    producer->purge(RdKafka::Producer::PURGE_QUEUE | RdKafka::Producer::PURGE_INFLIGHT);
    producer->flush(kPurgeDrainMs);
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Cancelled, "Cancelled", {}}}));
    return;
  }
  if (timedOut) {
    // Drain whatever's left so the producer can be destroyed cleanly, but report this as a plain Timeout
    // regardless of what drCb ends up capturing — "Local: Purged in queue" would be a confusing message for
    // what the user experiences as "it timed out" (the purge is just how we unblock the wait, not the cause).
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

// ---- Consumer (streaming, spec §5/§6) ----
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

  // BUG FIX: a wrong/unreachable bootstrap never surfaces as a consume() error — consume() just keeps
  // returning ERR__TIMED_OUT (empty poll), which the loop below treats as "tail hết dữ liệu" and spins on
  // forever, so the consumer never closed itself (Cancel was the only way out). Validate connectivity
  // up-front with a bounded metadata() call — the shared per-request Config-tab timeout, same value the
  // producer's delivery deadline honors — so a bad broker URL fails fast and the consumer closes cleanly
  // (SPEC_kafka §7 "Network: không kết nối được broker").
  //
  // BUG FIX (Cancel priority): metadata() as ONE blocking call held this thread for the whole Config-tab
  // timeout with no way to be interrupted — pressing Cancel while "connecting" did nothing until the
  // timeout expired (same shape as the producer's old un-sliced flush()). Slice the wait instead, checking
  // the token between slices, so Cancel wins within kCancelSliceMs no matter how long the timeout is.
  //
  // ONE deadline bounds the whole session (connect + consume): timeout_ms is the Config-tab request
  // timeout, so the outcome contract is phase-based — no broker within the deadline = FAILURE (Network),
  // Cancel while still connecting = FAILURE (Cancelled: the user never got a live consumer), while
  // deadline/Cancel AFTER the consumer is live = SUCCESS (EvClosed: a bounded tail that ends is the
  // feature, not an error).
  const auto deadline = std::chrono::steady_clock::now() + requestTimeout;
  {
    // "Still connecting" errors — one metadata slice against a down/unreachable/starting broker returns
    // TRANSPORT (refused / blackhole) or RESOLVE, NOT TIMED_OUT; librdkafka keeps reconnecting underneath,
    // so these are retryable until the deadline (a broker that is still starting up comes right up), not
    // instant failures. Anything else (auth, fatal) is a real error and ends the connect phase at once.
    const auto stillConnecting = [](RdKafka::ErrorCode e) {
      return e == RdKafka::ERR__TIMED_OUT || e == RdKafka::ERR__TRANSPORT ||
             e == RdKafka::ERR__ALL_BROKERS_DOWN || e == RdKafka::ERR__RESOLVE;
    };
    RdKafka::ErrorCode cerr;
    while (true) {
      if (cancel.cancelled()) {
        // Settle the stream FIRST — close() against an unreachable broker can itself block, and the user
        // already asked to stop; they must not wait on tear-down. Cancelled before the consumer ever went
        // live -> the session FAILED (EvFailed, not a graceful EvClosed).
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
      // Deadline expired while still connecting -> we never reached any broker — report it as Network
      // regardless of which connectivity code the last slice returned (a plain "Local: Timed out" would
      // read as an unhelpful protocol error).
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
  // BUG FIX (Cancel priority): consume(pollTimeout) blocked the token check for the WHOLE poll timeout —
  // a large Config-tab pollTimeout made Cancel appear dead. Cap each wait at kCancelSliceMs; observable
  // behavior is identical (consume() returns as soon as a record arrives, and a TIMED_OUT slice is already
  // treated as "no data yet"), but the token is re-checked at least every slice.
  const int pollMs = static_cast<int>(spec.config.pollTimeout.count());
  const int sliceMs = std::min(pollMs, kCancelSliceMs);
  // BUG FIX: the loop used to run unbounded — timeout_ms only ever bounded the connect probe, so a live
  // consumer ignored the Config-tab timeout entirely and Cancel was the only way out. The session deadline
  // (see above) now also bounds the tail: expiring here is the bounded-tail SUCCESS path (EvClosed
  // "timeout"), unlike expiring during connect which is a Network FAILURE.
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
        // Confluent-Avro auto-detect (SPEC_kafka Avro v1): magic 0x00 + schema id + a configured
        // registry -> decode for display; any failure degrades to verbatim bytes + a note (the
        // display codec renders non-UTF8 safely). One blocking fetch per NEW schema id, ~<=5s,
        // on this tail's own StreamPool thread; negative cache bounds a down registry.
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
        // Header values are arbitrary bytes and may be null — value_string() returns NULL for a null
        // value (UB in std::string) and truncates at embedded '\0'; copy length-based instead.
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
      // tail hết dữ liệu hiện có / poll rỗng — KHÔNG phải lỗi (spec §5/§7).
    } else {
      auto m = mapConsumeErr(merr);
      sink.emit(d::ResponseEvent(d::EvFailed{{m.kind, m.message, {}}}));
      consumer->close();
      return;
    }
    if (spec.config.maxMessages && count >= *spec.config.maxMessages) break;
  }

  bool cancelled = cancel.cancelled();
  // BUG FIX (Cancel priority): EvClosed used to be emitted AFTER close(), and close() (leave group +
  // final offset commit) can block for seconds — the UI stayed "sending" long after Cancel was pressed.
  // Settle the stream first; the group tear-down then finishes in the background of this call.
  // All three exits (Cancel mid-consume, session deadline, maxMessages) are graceful EvClosed — the
  // consumer WAS live, so ending the tail is success (contrast the connect phase above, where both map
  // to EvFailed).
  sink.emit(d::ResponseEvent(
      d::EvClosed{std::nullopt, cancelled ? "cancelled" : timedOut ? "timeout" : "stopped"}));
  consumer->close(); // commits final offsets if auto-commit is on
}

} // namespace

d::Status KafkaSender::execute(const d::RequestModel &resolved, d::IResponseSink &sink,
                               const d::ICancellationToken &cancel) {
  if (resolved.type() != d::RequestType::Kafka) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Unsupported, "not a kafka request", {}}}));
    return d::ok();
  }
  const d::KafkaRequest &k = std::get<d::KafkaRequest>(resolved.payload());
  const std::string bootstrap = k.brokers().toBootstrapServers();
  const auto requestTimeout = resolved.config().timeout.value();
  k.match(overloaded{
      [&](const d::KafkaProduceSpec &p) {
        produce(bootstrap, k.security(), p, sink, cancel, requestTimeout, registry_);
      },
      [&](const d::KafkaConsumeSpec &c) {
        consume(bootstrap, k.security(), c, sink, cancel, requestTimeout, registry_);
      },
  });
  return d::ok();
}

} // namespace core::infra
