// Turns store exceptions into Result errors at the boundary — no throw across layers.
#pragma once

#include <memory>
#include <string>

#include "core/domain/ports/driven/i_collection_repository.hpp"
#include "core/infra/persistence/stores.hpp"

namespace core::infra {

class CollectionRepository final : public domain::ICollectionRepository {
public:
  explicit CollectionRepository(std::shared_ptr<core::CollectionStore> store)
      : store_(std::move(store)) {}

  domain::Result<domain::RequestModel> load(const std::string &relPath) const override;
  domain::Result<std::string> save(const std::string &relPath,
                                   const domain::RequestModel &) const override;
  std::vector<domain::CollectionNode> listLevel(const std::string &dirRelPath) const override;
  domain::Status remove(const std::string &relPath) const override;

private:
  std::shared_ptr<core::CollectionStore> store_;
};

} // namespace core::infra
