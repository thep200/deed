// core/domain/grpc/proto_source.hpp — ProtoSource sum type (REFACTOR_SPEC §5.5):
// how the gRPC sender obtains method descriptors — server Reflection, local .proto files, or a descriptor set.
#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/domain/common/result.hpp"

namespace core::domain {

struct ProtoReflection {
  bool operator==(const ProtoReflection &) const { return true; }
};
struct ProtoFiles {
  std::vector<std::string> importPaths;
  std::vector<std::string> protoFiles;
  bool operator==(const ProtoFiles &o) const {
    return importPaths == o.importPaths && protoFiles == o.protoFiles;
  }
};
struct ProtoDescriptorSet {
  std::string descriptorSetPath;
  bool operator==(const ProtoDescriptorSet &o) const {
    return descriptorSetPath == o.descriptorSetPath;
  }
};

class ProtoSource {
public:
  using Variant = std::variant<ProtoReflection, ProtoFiles, ProtoDescriptorSet>;

  static ProtoSource reflection() { return ProtoSource(ProtoReflection{}); }
  static Result<ProtoSource> files(std::vector<std::string> importPaths,
                                   std::vector<std::string> protoFiles) {
    if (protoFiles.empty())
      return Result<ProtoSource>::fail(
          {ErrorCode::Validation, "protoFiles must list at least one .proto", "protoSource.protoFiles"});
    return Result<ProtoSource>::ok(
        ProtoSource(ProtoFiles{std::move(importPaths), std::move(protoFiles)}));
  }
  static Result<ProtoSource> descriptorSet(std::string path) {
    if (path.empty())
      return Result<ProtoSource>::fail(
          {ErrorCode::Validation, "descriptorSet path required", "protoSource.descriptorSetPath"});
    return Result<ProtoSource>::ok(ProtoSource(ProtoDescriptorSet{std::move(path)}));
  }

  template <class V> decltype(auto) match(V &&v) const { return std::visit(std::forward<V>(v), data_); }

  bool operator==(const ProtoSource &o) const { return data_ == o.data_; }
  bool operator!=(const ProtoSource &o) const { return !(data_ == o.data_); }

private:
  explicit ProtoSource(Variant v) : data_(std::move(v)) {}
  Variant data_;
};

} // namespace core::domain
