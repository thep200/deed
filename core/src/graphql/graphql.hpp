// graphql.hpp — GraphQL application layer helpers (SPEC_graphql §1-§4). INTERNAL (core/src).
// GraphQL adds NO transport: query/mutation -> HTTP POST (reuse HttpSender), subscription -> WS/SSE.
#pragma once

#include "core/types.hpp"

namespace core::gql {

// The effective operation: the request's declared one, or — when Auto — inferred from the query text
// (leading keyword query/mutation/subscription; a `{ … }` shorthand is a query). (SPEC_graphql §2)
GqlOperation effectiveOperation(const GraphQlRequest& g);

// Build an equivalent HTTP RequestModel for a query/mutation: POST <url> with body
// {query, variables, operationName} + Accept: application/graphql-response+json (or GET when configured).
// `g.variablesJson` must already be resolved ({{var}} expanded) so it parses as JSON. (SPEC_graphql §4)
RequestModel buildHttpModel(const RequestModel& model);

} // namespace core::gql
