// core/app/persistence_repositories.hpp — repository PORTS over the remaining stores (REFACTOR_SPEC §6.3/§8.3)
// so the UI can reach environments / session / app-config through interfaces instead of Engine directly.
// Transitional (app layer): returns the existing POD config types (core::Environment/Session/AppConfig from
// env_config.hpp — clean value types, not part of types.hpp's transport model). Header-only forwarders.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/infra/cache/cache.hpp"             // ResponseRecord (cache repo getResponse) — holds domain ApiResponse
#include "core/domain/request/request_model.hpp" // domain RequestModel (collection repo load/save/create)
#include "core/domain/response/api_error.hpp"    // domain ApiError (cache repo putError)
#include "core/domain/response/api_response.hpp" // domain ApiResponse (cache repo putResponse)
#include "core/env_config.hpp"        // TreeNode (collection tree) + the config PODs
#include "core/infra/persistence/stores.hpp"
#include "core/request_type.hpp"      // RequestType (createRequest) — survives types.hpp removal

namespace core::app {

// ---- Response cache ----
// App-layer repository over the response cache. Speaks DOMAIN ApiResponse/ApiError (REFACTOR_SPEC D); the
// ResponseRecord it returns embeds a domain ApiResponse. The concrete adapter (NativeResponseCacheRepository,
// owning a ResponseCache) is defined in composition_root.cpp; the header declares only the interface.
class IResponseCacheRepository {
public:
  virtual ~IResponseCacheRepository() = default;
  virtual void putResponse(const std::string &id, const core::domain::ApiResponse &) = 0;
  virtual void putResponse(const std::string &id, core::domain::ApiResponse &&) = 0;
  virtual void putError(const std::string &id, const core::domain::ApiError &) = 0;
  virtual std::optional<core::ResponseRecord> getResponse(const std::string &id) = 0;
  virtual void removeResponse(const std::string &id) = 0;
  virtual void reloadCacheConfig() = 0;
  virtual void flush() = 0;
  virtual std::uint64_t l1UsedBytes() const = 0; // 0 when the cache is disabled
};

// ---- Collection ----
// App-layer repository over CollectionStore. load/save/create-from-model speak the DOMAIN RequestModel
// (REFACTOR_SPEC P6 — the UI editor holds a domain model); scan/create/tree ops use the surviving
// TreeNode/RequestType view types. The store speaks domain natively, so the concrete adapter (in
// composition_root.cpp) just forwards — no conversion. The header declares only the interface.
class ICollectionRepository {
public:
  virtual ~ICollectionRepository() = default;
  virtual std::vector<core::TreeNode> scanLevel(const std::string &dirRelPath) const = 0;
  virtual core::TreeNode scanTree() const = 0;
  virtual core::domain::RequestModel loadRequest(const std::string &relPath) const = 0;
  virtual std::string saveRequest(const std::string &relPath, const core::domain::RequestModel &) const = 0;
  virtual std::string createRequest(const std::string &folderRel, core::RequestType,
                                    const std::string &name) const = 0;
  virtual std::string createRequestFromModel(const std::string &folderRel, core::domain::RequestModel model,
                                             const std::string &name) const = 0;
  virtual std::string createFolder(const std::string &parentRel, const std::string &name) const = 0;
  virtual std::string rename(const std::string &relPath, const std::string &newName) const = 0;
  virtual std::string duplicate(const std::string &relPath) const = 0;
  virtual void remove(const std::string &relPath) const = 0;
  virtual std::string move(const std::string &relPath, const std::string &destFolderRel) const = 0;
  virtual std::string findRelPathById(const std::string &id) const = 0;
  virtual int migrateAddIdToFilenames() const = 0;
};

// ---- Environments ----
class IEnvironmentRepository {
public:
  virtual ~IEnvironmentRepository() = default;
  virtual std::vector<std::string> list() const = 0;
  virtual core::Environment load(const std::string &name) const = 0;
  virtual void save(const core::Environment &) = 0;
  virtual void remove(const std::string &name) = 0;
};
class EnvironmentRepository final : public IEnvironmentRepository {
public:
  explicit EnvironmentRepository(core::EnvironmentStore &s) : s_(s) {}
  std::vector<std::string> list() const override { return s_.list(); }
  core::Environment load(const std::string &n) const override { return s_.load(n); }
  void save(const core::Environment &e) override { s_.save(e); }
  void remove(const std::string &n) override { s_.remove(n); }

private:
  core::EnvironmentStore &s_;
};

// ---- Session ----
class ISessionRepository {
public:
  virtual ~ISessionRepository() = default;
  virtual core::Session load() const = 0;
  virtual std::string loadLastOpened() const = 0;
  virtual void saveLastOpened(const std::string &relPath) = 0;
  virtual std::string getActiveEnv() const = 0;
  virtual void setActiveEnv(const std::string &name) = 0;
};
class SessionRepository final : public ISessionRepository {
public:
  explicit SessionRepository(core::SessionStore &s) : s_(s) {}
  core::Session load() const override { return s_.load(); }
  std::string loadLastOpened() const override { return s_.loadLastOpened(); }
  void saveLastOpened(const std::string &rel) override { s_.saveLastOpened(rel); }
  std::string getActiveEnv() const override { return s_.getActiveEnv(); }
  void setActiveEnv(const std::string &n) override { s_.setActiveEnv(n); }

private:
  core::SessionStore &s_;
};

// ---- App config ----
class IAppConfigRepository {
public:
  virtual ~IAppConfigRepository() = default;
  virtual core::AppConfig load() const = 0;
  virtual void save(const core::AppConfig &) = 0;
};
class AppConfigRepository final : public IAppConfigRepository {
public:
  explicit AppConfigRepository(core::AppConfigStore &s) : s_(s) {}
  core::AppConfig load() const override { return s_.load(); }
  void save(const core::AppConfig &c) override { s_.save(c); }

private:
  core::AppConfigStore &s_;
};

} // namespace core::app
