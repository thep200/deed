#pragma once

#include <string>

#include "core/domain/common/result.hpp"
#include "core/domain/graphql/gql_schema.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::gql {

// The classic (pre-2021) standard introspection query — no specifiedByURL/isRepeatable, so older servers
// that reject unknown introspection fields still answer.
const std::string &introspectionQuery();

// Accepts {"data":{"__schema":…}} or a bare {"__schema":…}; a GraphQL errors[] body or missing __schema fails.
core::domain::Result<std::string> sdlFromIntrospectionJson(const std::string &body);

// POSTs with the request's own headers/auth/config; ws(s):// rewrites to http(s). `resolved` must already
// have {{var}} substituted. Synchronous — blocks the calling thread.
core::domain::Result<core::domain::GqlSchema> runIntrospection(const core::domain::RequestModel &resolved);

} // namespace core::gql
