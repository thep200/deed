// schema_registry_client.hpp — Confluent Schema Registry REST client (SPEC_kafka Avro v1).
// INTERNAL (core/src). Thread-safe: one instance lives in KafkaSender and is shared by concurrent
// StreamPool tails. nlohmann stays in the .cpp.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "core/domain/common/result.hpp"
#include "core/domain/kafka/kafka_common.hpp"
#include "core/domain/ports/driven/i_cancellation_token.hpp"

namespace core::infra {

// Pure response parsers, exposed for unit tests (bodies per the v1 REST API):
struct RegisteredSchema {
  std::int32_t id = 0;
  std::string schemaJson;
};
// {"id": 7, "subject": ..., "version": ..., "schema": "<JSON-escaped schema string>"}
core::domain::Result<RegisteredSchema> parseLatestSubjectResponse(const std::string &body);
// {"schema": "<JSON-escaped schema string>"}
core::domain::Result<std::string> parseSchemaByIdResponse(const std::string &body);

class SchemaRegistryClient {
public:
  // GET {url}/subjects/{subject}/versions/latest. NO cache — a produce is one user click and
  // freshness wins (the subject's latest schema may have just changed).
  core::domain::Result<RegisteredSchema>
  latestForSubject(const core::domain::SchemaRegistryRef &ref, const std::string &subject,
                   const core::domain::ICancellationToken &cancel);

  // GET {url}/schemas/ids/{id}. Positive cache forever (ids are immutable); negative cache 30s so a
  // down registry costs at most one fetch per id per 30s while records keep flowing.
  core::domain::Result<std::string> schemaById(const core::domain::SchemaRegistryRef &ref,
                                               std::int32_t id,
                                               const core::domain::ICancellationToken &cancel);

private:
  core::domain::Result<std::string> get(const core::domain::SchemaRegistryRef &ref,
                                        const std::string &path,
                                        const core::domain::ICancellationToken &cancel);

  std::mutex mu_;
  std::unordered_map<std::string, std::string> byId_; // key = url + "|" + id
  std::unordered_map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>>
      negById_; // failure message + expiry
};

} // namespace core::infra
