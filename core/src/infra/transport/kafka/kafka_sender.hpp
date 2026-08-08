// <rdkafkacpp.h> is confined to src/infra/transport/kafka (layering_gate). Stateless across calls: each
// execute() owns its producer/consumer; no push/close — the consumer loop cooperatively polls the token.
#pragma once

#include "infra/transport/kafka/schema_registry_client.hpp"
#include "infra/transport/typed_sender.hpp"

namespace core::infra {

class KafkaSender final : public TypedSender<domain::KafkaRequest> {
protected:
  domain::Status executeTyped(const domain::RequestModel &resolved,
                              const domain::KafkaRequest &kafka, domain::IResponseSink &sink,
                              const domain::ICancellationToken &cancel) override;
  const char *mismatchMessage() const override { return "not a kafka request"; }

private:
  // Shared across calls so schema-by-id lookups cache across tails; internally mutexed.
  SchemaRegistryClient registry_;
};

} // namespace core::infra
