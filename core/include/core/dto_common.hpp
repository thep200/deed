// core/dto_common.hpp — primitives shared by every neutral DTO (README §7). Keep tiny + dependency-free.
#pragma once

#include <string>
#include <vector>

namespace core {

// key/value line (headers, params, metadata, pathVariables, form...). Array (not object) to keep order,
// allow duplicate keys, each line has `enabled`. (README §7.4)
struct KeyValue {
  std::string key;
  std::string value;
  bool enabled = true;
};

struct MultipartPart {
  std::string key;
  std::string value;    // used when type == "text"
  std::string type;     // "text" | "file"
  std::string filePath; // used when type == "file"
  bool enabled = true;
};

} // namespace core
