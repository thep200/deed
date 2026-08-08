#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/request/request_id.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::domain {

// relPath is the opaque persistence key.
struct CollectionNode {
  bool isFolder = false;
  std::string relPath;
  std::string name;
  std::optional<RequestId> id;     // requests only
  std::optional<RequestType> type; // requests only
};

class ICollectionRepository {
public:
  virtual ~ICollectionRepository() = default;

  virtual Result<RequestModel> load(const std::string &relPath) const = 0;
  // Returns the (possibly renamed) relPath after write.
  virtual Result<std::string> save(const std::string &relPath, const RequestModel &) const = 0;
  virtual std::vector<CollectionNode> listLevel(const std::string &dirRelPath) const = 0;
  virtual Status remove(const std::string &relPath) const = 0;
};

} // namespace core::domain
