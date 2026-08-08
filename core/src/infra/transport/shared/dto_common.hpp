// Keep tiny and dependency-free.
#pragma once

#include <string>
#include <vector>

namespace core {

// Serialized as an array (not an object) to keep order and allow duplicate keys.
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
