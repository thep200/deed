// kafka_sender.cpp — Kafka producer+consumer over librdkafka (SPEC_kafka §6/§7). ONLY file allowed to
// include <rdkafkacpp.h> (layering_gate enforces this — confined to src/infra/transport/kafka).
#include "infra/transport/kafka/kafka_sender.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp> // producer delivery summary (body text) — infra/transport may use it (not domain)
#include <librdkafka/rdkafkacpp.h>

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

// ---- Error mapping (SPEC_kafka §7) ----
struct MappedErr {
  d::ErrorKind kind;
  std::string message;
};

MappedErr mapProduceErr(RdKafka::ErrorCode err) {
  switch (err) {
  case RdKafka::ERR__MSG_TIMED_OUT:
    return {d::ErrorKind::Timeout, "Delivery timed out — is the broker running?"};
  case RdKafka::ERR__TRANSPORT:
  case RdKafka::ERR__ALL_BROKERS_DOWN:
  case RdKafka::ERR__RESOLVE:
    return {d::ErrorKind::Network, "Could not connect to the broker — check host:port and whether Kafka has started."};
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
    return {d::ErrorKind::Network, "Could not connect to the broker — check host:port and whether Kafka has started."};
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
            d::IResponseSink &sink, const d::ICancellationToken &cancel, std::chrono::milliseconds requestTimeout) {
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
  void *payload = const_cast<char *>(spec.message.value.value.data());
  size_t payloadLen = spec.message.value.value.size();
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
    sink.emit(d::ResponseEvent(
        d::EvFailed{{d::ErrorKind::Timeout, "Delivery timed out — is the broker running?", {}}}));
    return;
  }

  if (!drCb.got) {
    sink.emit(d::ResponseEvent(
        d::EvFailed{{d::ErrorKind::Timeout, "Delivery timed out — is the broker running?", {}}}));
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
            d::IResponseSink &sink, const d::ICancellationToken &cancel, std::chrono::milliseconds requestTimeout) {
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
  {
    RdKafka::Metadata *rawMd = nullptr;
    RdKafka::ErrorCode cerr =
        consumer->metadata(true, nullptr, &rawMd, static_cast<int>(requestTimeout.count()));
    std::unique_ptr<RdKafka::Metadata> md(rawMd); // owns the reply; librdkafka hands us a heap Metadata
    if (cerr != RdKafka::ERR_NO_ERROR) {
      // A metadata timeout here means we never reached any broker — report it as Network (a plain
      // "Local: Timed out" would read as an unhelpful protocol error) rather than mapConsumeErr's default.
      MappedErr m = cerr == RdKafka::ERR__TIMED_OUT
                        ? MappedErr{d::ErrorKind::Network,
                                    "Could not connect to the broker — check host:port and whether Kafka "
                                    "has started."}
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
  const int pollMs = static_cast<int>(spec.config.pollTimeout.count());
  while (!cancel.cancelled()) {
    std::unique_ptr<RdKafka::Message> msg(consumer->consume(pollMs));
    RdKafka::ErrorCode merr = msg->err();
    if (merr == RdKafka::ERR_NO_ERROR) {
      d::KafkaRecord rec;
      rec.topic = msg->topic_name();
      rec.partition = msg->partition();
      rec.offset = msg->offset();
      if (msg->key()) rec.key = *msg->key();
      if (msg->payload() != nullptr && msg->len() > 0) {
        rec.value.assign(static_cast<const char *>(msg->payload()), msg->len());
      } else {
        rec.valueIsNull = true;
      }
      if (auto *hdrs = msg->headers())
        for (const auto &h : hdrs->get_all()) rec.headers.push_back({h.key(), h.value_string(), true});
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
  consumer->close(); // commits final offsets if auto-commit is on
  sink.emit(d::ResponseEvent(d::EvClosed{std::nullopt, cancelled ? "cancelled" : "stopped"}));
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
      [&](const d::KafkaProduceSpec &p) { produce(bootstrap, k.security(), p, sink, cancel, requestTimeout); },
      [&](const d::KafkaConsumeSpec &c) { consume(bootstrap, k.security(), c, sink, cancel, requestTimeout); },
  });
  return d::ok();
}

} // namespace core::infra
