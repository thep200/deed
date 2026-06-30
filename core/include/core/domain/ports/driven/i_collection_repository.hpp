// core/domain/ports/driven/i_collection_repository.hpp — persistence as a port returning DOMAIN objects
// (REFACTOR_SPEC §6.3/§8.3). The repository hides JSON + on-disk layout; callers see only domain values.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/common/result.hpp"
#include "core/domain/request/request_id.hpp"
#include "core/domain/request/request_model.hpp"

namespace core::domain {

// One entry in a collection tree level (folder or request) — relPath is the opaque persistence key.
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
  // Persist; returns the (possibly renamed) relPath after write.
  virtual Result<std::string> save(const std::string &relPath, const RequestModel &) const = 0;
  virtual std::vector<CollectionNode> listLevel(const std::string &dirRelPath) const = 0;
  virtual Status remove(const std::string &relPath) const = 0;
};

} // namespace core::domain
