#pragma once

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/common/result.hpp"

namespace core::domain {

struct MetadataEntry {
  std::string key;
  std::string value;
  bool enabled = true;
  bool operator==(const MetadataEntry &o) const {
    return key == o.key && value == o.value && enabled == o.enabled;
  }
};

// gRPC metadata key rules (HTTP/2 header semantics): lowercase ASCII letters/digits and `-_.`, no spaces.
// A `-bin` suffix marks a binary value; we accept it (binary encoding is the sender's concern).
class GrpcMetadata {
public:
  static GrpcMetadata empty() { return GrpcMetadata({}); }

  static Result<GrpcMetadata> create(std::vector<MetadataEntry> entries) {
    for (const auto &e : entries) {
      if (!e.enabled) continue;
      if (e.key.empty())
        return Result<GrpcMetadata>::fail(
            {ErrorCode::Validation, "metadata key must not be empty", "metadata.key"});
      if (!isValidKey(e.key))
        return Result<GrpcMetadata>::fail(
            {ErrorCode::Validation, "invalid metadata key: " + e.key, "metadata.key"});
    }
    return Result<GrpcMetadata>::ok(GrpcMetadata(std::move(entries)));
  }

  const std::vector<MetadataEntry> &entries() const noexcept { return entries_; }
  bool empty_() const noexcept { return entries_.empty(); }

  bool operator==(const GrpcMetadata &o) const { return entries_ == o.entries_; }
  bool operator!=(const GrpcMetadata &o) const { return !(*this == o); }

private:
  explicit GrpcMetadata(std::vector<MetadataEntry> e) : entries_(std::move(e)) {}

  static bool isValidKey(const std::string &k) {
    for (unsigned char c : k) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
      if (!ok) return false;
    }
    return true;
  }

  std::vector<MetadataEntry> entries_;
};

} // namespace core::domain
