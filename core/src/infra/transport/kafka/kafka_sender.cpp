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
    return {d::ErrorKind::Timeout, "Hết thời gian chờ delivery — broker có chạy không?"};
  case RdKafka::ERR__TRANSPORT:
  case RdKafka::ERR__ALL_BROKERS_DOWN:
  case RdKafka::ERR__RESOLVE:
    return {d::ErrorKind::Network, "Không nối được broker — kiểm tra host:port và Kafka đã start chưa."};
  case RdKafka::ERR_UNKNOWN_TOPIC_OR_PART:
    return {d::ErrorKind::Protocol, "Topic chưa tồn tại và auto-create tắt — tạo trước."};
  case RdKafka::ERR_LEADER_NOT_AVAILABLE:
  case RdKafka::ERR_NOT_LEADER_FOR_PARTITION:
    return {d::ErrorKind::Protocol, "Partition leader chưa sẵn sàng — thử lại sau."};
  case RdKafka::ERR_MSG_SIZE_TOO_LARGE:
    return {d::ErrorKind::Protocol, "Message vượt message.max.bytes của broker/topic."};
  default: return {d::ErrorKind::Protocol, RdKafka::err2str(err)};
  }
}
MappedErr mapConsumeErr(RdKafka::ErrorCode err) {
  switch (err) {
  case RdKafka::ERR__TRANSPORT:
  case RdKafka::ERR__ALL_BROKERS_DOWN:
  case RdKafka::ERR__RESOLVE:
    return {d::ErrorKind::Network, "Không nối được broker — kiểm tra host:port và Kafka đã start chưa."};
  case RdKafka::ERR_UNKNOWN_TOPIC_OR_PART:
    return {d::ErrorKind::Protocol, "Topic chưa tồn tại — tạo trước hoặc bật auto-create."};
  case RdKafka::ERR_GROUP_AUTHORIZATION_FAILED:
  case RdKafka::ERR_TOPIC_AUTHORIZATION_FAILED:
    return {d::ErrorKind::Protocol,
            "Không đủ quyền group/topic — kiểm tra ACL (hoặc để trống group để tail ẩn danh)."};
  case RdKafka::ERR__FATAL:
    return {d::ErrorKind::Internal, "Consumer gặp lỗi fatal — xem log; thử Start lại."};
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
            d::IResponseSink &sink) {
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
  bool tombstone = spec.message.value.tombstone;
  void *payload = tombstone ? nullptr : const_cast<char *>(spec.message.value.value.data());
  size_t payloadLen = tombstone ? 0 : spec.message.value.value.size();
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

  producer->flush(static_cast<int>(spec.config.messageTimeout.count()));

  if (!drCb.got) {
    sink.emit(d::ResponseEvent(
        d::EvFailed{{d::ErrorKind::Timeout, "Hết thời gian chờ delivery — broker có chạy không?", {}}}));
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
            d::IResponseSink &sink, const d::ICancellationToken &cancel) {
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
  k.match(overloaded{
      [&](const d::KafkaProduceSpec &p) { produce(bootstrap, k.security(), p, sink); },
      [&](const d::KafkaConsumeSpec &c) { consume(bootstrap, k.security(), c, sink, cancel); },
  });
  return d::ok();
}

} // namespace core::infra
