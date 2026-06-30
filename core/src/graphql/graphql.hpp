// graphql.hpp — GraphQL application layer helpers (SPEC_graphql §1-§4). INTERNAL (core/src).
// GraphQL adds NO transport: query/mutation -> HTTP POST (reuse the native HTTP sender), subscription -> WS.
// Operates on the DOMAIN GraphQlRequest/RequestModel (no legacy structs) so the GraphQL sender is bridge-free.
#pragma once

#include "core/domain/graphql/graphql_request.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::gql {

// The effective operation: the request's declared one, or — when Auto — inferred from the query text
// (leading keyword query/mutation/subscription; a `{ … }` shorthand is a query). (SPEC_graphql §2)
core::domain::GqlOperationType effectiveOperation(const core::domain::GraphQlRequest& g);

// Build an equivalent HTTP RequestModel for a query/mutation: POST <url> with body
// {query, variables, operationName} + Accept: application/graphql-response+json (SPEC_graphql §4).
// `g.op().variables` must already be resolved ({{var}} expanded) so it parses as JSON. The envelope
// (id/name/seq/config) is carried over from `model`.
core::domain::RequestModel buildHttpModel(const core::domain::RequestModel& model);

} // namespace core::gql
