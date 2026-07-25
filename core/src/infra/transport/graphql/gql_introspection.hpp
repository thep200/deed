// gql_introspection.hpp — GraphQL schema introspection (the Schema response tab). INTERNAL (core/src).
// One-off HTTP POST of the standard introspection query + SDL rendering of the reply. nlohmann stays in
// the .cpp (same visibility convention as gql_payload.hpp).
#pragma once

#include <string>

#include "core/domain/common/result.hpp"
#include "core/domain/graphql/gql_schema.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::gql {

// The classic (pre-2021) standard introspection query — no specifiedByURL/isRepeatable, so older servers
// that reject unknown introspection fields still answer.
const std::string &introspectionQuery();

// Pure: introspection response body (JSON text) -> SDL. Accepts {"data":{"__schema":…}} or a bare
// {"__schema":…}; a GraphQL errors[] body or a missing __schema is a failure. Unit-tested.
core::domain::Result<std::string> sdlFromIntrospectionJson(const std::string &body);

// POST the introspection query to the request's endpoint with the request's own headers/auth/config
// (ws:// and wss:// URLs are rewritten to http(s) — introspection is always HTTP). `resolved` must
// already have {{var}} substituted. Synchronous — blocks the calling thread (the UI calls it off-main).
core::domain::Result<core::domain::GqlSchema> runIntrospection(const core::domain::RequestModel &resolved);

} // namespace core::gql
