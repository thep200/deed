#include "infra/transport/kafka/kafka_conf.hpp"

#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

namespace core::infra::kafka_detail {

void setConf(RdKafka::Conf *conf, const std::string &key, const std::string &val) {
  std::string err;
  conf->set(key, val, err); // best-effort: a bad extra[] property is the user's problem, not fatal here
}
void applyExtra(RdKafka::Conf *conf, const std::vector<d::KafkaExtra> &extra) {
  for (const auto &e : extra)
    if (e.enabled) setConf(conf, e.key, e.value);
}
// PLAINTEXT is rdkafka's default (no property to set); seam for SSL/SASL alternatives.
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

} // namespace core::infra::kafka_detail
