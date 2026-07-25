// core/domain/graphql/gql_schema.hpp — introspection result VO: two views of the same server schema
// (SDL text for reading, pretty introspection JSON for the raw toggle). Pure data, filled by infra.
#pragma once

#include <string>

namespace core::domain {

struct GqlSchema {
  std::string sdl;  // schema rendered as SDL
  std::string json; // pretty-printed introspection "data" object
  bool operator==(const GqlSchema &o) const { return sdl == o.sdl && json == o.json; }
  bool operator!=(const GqlSchema &o) const { return !(*this == o); }
};

} // namespace core::domain
