#include "infra/persistence/collection_repository.hpp"

#include <exception>

namespace core::infra {
namespace d = core::domain;

namespace {
d::RequestType toDomainType(core::RequestType t) {
  switch (t) {
  case core::RequestType::Grpc: return d::RequestType::Grpc;
  case core::RequestType::WebSocket: return d::RequestType::WebSocket;
  case core::RequestType::GraphQL: return d::RequestType::GraphQl;
  default: return d::RequestType::Http;
  }
}
} // namespace

d::Result<d::RequestModel> CollectionRepository::load(const std::string &relPath) const {
  try {
    return d::Result<d::RequestModel>::ok(store_->loadRequest(relPath)); // store speaks domain now
  } catch (const std::exception &e) {
    return d::Result<d::RequestModel>::fail({d::ErrorCode::NotFound, e.what(), relPath});
  }
}

d::Result<std::string> CollectionRepository::save(const std::string &relPath,
                                                  const d::RequestModel &model) const {
  try {
    return d::Result<std::string>::ok(store_->saveRequest(relPath, model));
  } catch (const std::exception &e) {
    return d::Result<std::string>::fail({d::ErrorCode::Internal, e.what(), relPath});
  }
}

std::vector<d::CollectionNode> CollectionRepository::listLevel(const std::string &dirRelPath) const {
  std::vector<d::CollectionNode> out;
  for (const auto &n : store_->scanLevel(dirRelPath)) {
    d::CollectionNode node;
    node.isFolder = n.isFolder;
    node.relPath = n.relPath;
    node.name = n.name;
    if (!n.isFolder) {
      node.id = d::RequestId(n.id);
      node.type = toDomainType(n.requestType);
    }
    out.push_back(std::move(node));
  }
  return out;
}

d::Status CollectionRepository::remove(const std::string &relPath) const {
  try {
    store_->remove(relPath);
    return d::ok();
  } catch (const std::exception &e) {
    return d::Status::fail({d::ErrorCode::Internal, e.what(), relPath});
  }
}

} // namespace core::infra
