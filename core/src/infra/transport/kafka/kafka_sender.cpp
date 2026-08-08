// <rdkafkacpp.h> stays confined to src/infra/transport/kafka (layering_gate).
#include "infra/transport/kafka/kafka_sender.hpp"

#include <string>

#include "infra/transport/kafka/kafka_conf.hpp"

namespace core::infra {
namespace d = core::domain;
using namespace kafka_detail;

d::Status KafkaSender::executeTyped(const d::RequestModel &resolved, const d::KafkaRequest &k,
                                    d::IResponseSink &sink, const d::ICancellationToken &cancel) {
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
