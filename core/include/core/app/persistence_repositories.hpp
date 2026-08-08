#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/infra/cache/cache.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/response/api_error.hpp"
#include "core/domain/response/api_response.hpp"
#include "core/domain/environment/env_config.hpp"
#include "core/infra/persistence/stores.hpp"
#include "core/domain/request/request_type.hpp"

namespace core::app {

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

class ICollectionRepository {
public:
  virtual ~ICollectionRepository() = default;
  virtual std::vector<core::TreeNode> scanLevel(const std::string &dirRelPath) const = 0;
  virtual core::TreeNode scanTree() const = 0;
  virtual core::domain::RequestModel loadRequest(const std::string &relPath) const = 0;
  virtual std::string saveRequest(const std::string &relPath, const core::domain::RequestModel &) const = 0;
  // UI-only per-mode body drafts: keep text typed in non-active body modes across save/reload; the domain model ignores them.
  virtual std::map<std::string, std::string> loadBodyDrafts(const std::string &relPath) const = 0;
  virtual std::string saveRequest(const std::string &relPath, const core::domain::RequestModel &,
                                  const std::map<std::string, std::string> &bodyDrafts) const = 0;
  virtual std::string createRequest(const std::string &folderRel, core::RequestType,
                                    const std::string &name) const = 0;
  virtual std::string createRequestFromModel(const std::string &folderRel, core::domain::RequestModel model,
                                             const std::string &name) const = 0;
  virtual std::string createFolder(const std::string &parentRel, const std::string &name) const = 0;
  virtual std::string rename(const std::string &relPath, const std::string &newName) const = 0;
  virtual std::string duplicate(const std::string &relPath) const = 0;
  virtual void remove(const std::string &relPath) const = 0;
  virtual std::string move(const std::string &relPath, const std::string &destFolderRel) const = 0;
  // Drop at a precise slot: renames ONE entry (fractional order key), siblings untouched.
  virtual std::string reorder(const std::string &relPath, const std::string &destFolderRel,
                              int index) const = 0;
  virtual std::string findRelPathById(const std::string &id) const = 0;
};

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
