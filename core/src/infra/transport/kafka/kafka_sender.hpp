// kafka_sender.hpp — Kafka producer+consumer transport over librdkafka (SPEC_kafka §6). INTERNAL
// (core/src): the ONLY place allowed to include <rdkafkacpp.h> (layering_gate enforces this). One sender,
// dispatch by which KafkaRequest::Mode alternative is held — mirrors WsSenderAdapter's shape, but stateless
// across calls (each execute() owns its own producer/consumer for the duration of the call; no push/close
// needed — the consumer loop is a cooperative poll against ICancellationToken, per spec §6/§8).
#pragma once

#include "core/domain/ports/driven/i_request_sender.hpp"
#include "infra/transport/kafka/schema_registry_client.hpp"

namespace core::infra {

class KafkaSender final : public domain::IRequestSender {
public:
  bool supports(domain::RequestType t) const override { return t == domain::RequestType::Kafka; }
  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;

private:
  // Shared across calls so schema-by-id lookups cache across tails; internally mutexed (the send
  // calls themselves stay stateless per the header note above).
  SchemaRegistryClient registry_;
};

} // namespace core::infra
