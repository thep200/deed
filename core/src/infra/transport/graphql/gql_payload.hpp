// Names nlohmann — do NOT include from graphql.hpp (app-visible, lib-free).
#pragma once

#include <nlohmann/json.hpp>

#include "core/domain/graphql/graphql_request.hpp"

namespace core::gql {

// {query, variables (empty/invalid JSON -> {}), operationName (omitted when empty)}.
nlohmann::json operationPayload(const core::domain::GraphQlOperation& op);

} // namespace core::gql
