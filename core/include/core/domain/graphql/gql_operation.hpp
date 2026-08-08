#pragma once

#include <cctype>
#include <cstddef>
#include <string>

#include "core/domain/graphql/graphql_request.hpp"

namespace core::domain {

// Declared operation, or — when Auto — inferred from the query text's leading keyword.
inline GqlOperationType effectiveOperation(const GraphQlRequest &g) {
  if (g.op().operation != GqlOperationType::Auto) return g.op().operation;

  const std::string &q = g.op().query;
  std::size_t i = 0;
  if (q.size() >= 3 && (unsigned char)q[0] == 0xEF && (unsigned char)q[1] == 0xBB && (unsigned char)q[2] == 0xBF)
    i = 3;
  while (i < q.size()) {
    char c = q[i];
    if (std::isspace((unsigned char)c) || c == ',') { ++i; continue; }
    if (c == '#') { while (i < q.size() && q[i] != '\n') ++i; continue; }   // comment to EOL
    break;
  }
  if (i >= q.size()) return GqlOperationType::Query;          // empty -> treat as query
  if (q[i] == '{') return GqlOperationType::Query;            // shorthand `{ … }` is a query

  std::string word;
  while (i < q.size() && (std::isalpha((unsigned char)q[i]))) word += q[i++];
  if (word == "mutation") return GqlOperationType::Mutation;
  if (word == "subscription") return GqlOperationType::Subscription;
  return GqlOperationType::Query;                             // "query" or anything else -> query
}

} // namespace core::domain
