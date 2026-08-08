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
