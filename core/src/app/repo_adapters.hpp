#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/cache_config.hpp"
#include "core/app/persistence_repositories.hpp"

namespace core::app {

std::int64_t nowEpochMs();

class NativeResponseCacheRepository final : public IResponseCacheRepository {
public:
  NativeResponseCacheRepository(core::AppConfigStore *appCfg, core::CacheLimits limits, std::string sessionDir)
      : appCfg_(appCfg), limits_(limits), sessionDir_(std::move(sessionDir)) {
    rebuild();
  }
  void putResponse(const std::string &id, const core::domain::ApiResponse &resp) override {
    putResponse(id, core::domain::ApiResponse(resp));
  }
  void putResponse(const std::string &id, core::domain::ApiResponse &&resp) override {
    if (id.empty()) return;
    auto c = cachePtr();
    if (!c) return;
    core::ResponseRecord rec;
    rec.isError = false;
    // Domain ApiResponse has no resolved-request dump to fingerprint -> requestRevision stays empty.
    rec.response = std::move(resp);
    rec.receivedAt = nowEpochMs();
    c->put(id, std::move(rec));
  }
  void putError(const std::string &id, const core::domain::ApiError &err) override {
    if (id.empty()) return;
    auto c = cachePtr();
    if (!c) return;
    core::ResponseRecord rec;
    rec.isError = true;
    rec.errorKind = err.kind;
    rec.errorMessage = err.message;
    rec.receivedAt = nowEpochMs();
    c->put(id, std::move(rec));
  }
  std::optional<core::ResponseRecord> getResponse(const std::string &id) override {
    auto c = cachePtr();
    return c ? c->get(id) : std::nullopt;
  }
  void removeResponse(const std::string &id) override {
    auto c = cachePtr();
    if (c) c->remove(id);
  }
  void reloadCacheConfig() override {
    std::shared_ptr<core::ResponseCache> c;
    core::CacheConfig fresh;
    {
      std::lock_guard<std::mutex> lk(mu_);
      bool wasPersist = cfg_.persist, wasEnabled = cfg_.enabled;
      fresh = core::detail::buildCacheConfig(appCfg_->load(), limits_);
      if (cache_ && fresh.enabled == wasEnabled && fresh.persist == wasPersist) {
        cfg_ = fresh;
        c = cache_; // change cap/threshold in place (keep tiers) -> onConfigChanged OUTSIDE the lock
      }
    }
    if (c) { c->onConfigChanged(fresh); return; }
    rebuild(); // toggle enabled / change persist -> rebuild tiers
  }
  void flush() override {
    auto c = cachePtr();
    if (c) c->flush();
  }
  std::uint64_t l1UsedBytes() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return cache_ ? cache_->l1UsedBytes() : 0;
  }

private:
  std::shared_ptr<core::ResponseCache> cachePtr() {
    std::lock_guard<std::mutex> lk(mu_);
    return cache_;
  }
  void rebuild() {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = core::detail::buildCacheConfig(appCfg_->load(), limits_);
    cache_ = cfg_.enabled ? std::shared_ptr<core::ResponseCache>(core::ResponseCache::create(cfg_, sessionDir_))
                          : nullptr;
  }
  core::AppConfigStore *appCfg_;
  core::CacheLimits limits_;
  std::string sessionDir_;
  mutable std::mutex mu_;
  core::CacheConfig cfg_;
  std::shared_ptr<core::ResponseCache> cache_;
};

class CollectionRepository final : public ICollectionRepository {
public:
  explicit CollectionRepository(core::CollectionStore &s) : s_(s) {}
  std::vector<core::TreeNode> scanLevel(const std::string &d) const override { return s_.scanLevel(d); }
  core::TreeNode scanTree() const override { return s_.scanTree(); }
  core::domain::RequestModel loadRequest(const std::string &r) const override { return s_.loadRequest(r); }
  std::string saveRequest(const std::string &r, const core::domain::RequestModel &m) const override {
    return s_.saveRequest(r, m);
  }
  std::map<std::string, std::string> loadBodyDrafts(const std::string &r) const override {
    return s_.loadBodyDrafts(r);
  }
  std::string saveRequest(const std::string &r, const core::domain::RequestModel &m,
                          const std::map<std::string, std::string> &drafts) const override {
    return s_.saveRequest(r, m, drafts);
  }
  std::string createRequest(const std::string &f, core::RequestType t, const std::string &n) const override {
    return s_.createRequest(f, t, n);
  }
  std::string createRequestFromModel(const std::string &f, core::domain::RequestModel m,
                                     const std::string &n) const override {
    return s_.createRequestFromModel(f, std::move(m), n);
  }
  std::string createFolder(const std::string &p, const std::string &n) const override {
    return s_.createFolder(p, n);
  }
  std::string rename(const std::string &r, const std::string &n) const override { return s_.rename(r, n); }
  std::string duplicate(const std::string &r) const override { return s_.duplicate(r); }
  void remove(const std::string &r) const override { s_.remove(r); }
  std::string move(const std::string &r, const std::string &d) const override { return s_.move(r, d); }
  std::string reorder(const std::string &r, const std::string &d, int i) const override {
    return s_.reorder(r, d, i);
  }
  std::string findRelPathById(const std::string &id) const override { return s_.findRelPathById(id); }

private:
  core::CollectionStore &s_;
};

} // namespace core::app
