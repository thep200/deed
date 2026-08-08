// GraphQL adds no transport of its own: query/mutation -> HTTP POST, subscription -> WS.
#pragma once

#include "core/domain/graphql/gql_operation.hpp"
#include "core/domain/graphql/graphql_request.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::gql {

// Declared operation, or — when Auto — inferred from the query text (a `{ … }` shorthand is a query).
inline core::domain::GqlOperationType effectiveOperation(const core::domain::GraphQlRequest& g) {
    return core::domain::effectiveOperation(g);
}

// POST <url> with {query, variables, operationName}. `g.op().variables` must already be {{var}}-resolved
// so it parses as JSON; the envelope (id/name/seq/config) carries over from `model`.
core::domain::RequestModel buildHttpModel(const core::domain::RequestModel& model);

} // namespace core::gql
