#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/app/persistence_repositories.hpp"
#include "core/domain/response/interaction.hpp"
#include "core/app/request_orchestrator.hpp"
#include "core/domain/ports/driving/i_api_client.hpp"
#include "core/domain/ports/driven/i_clock.hpp"
#include "core/domain/ports/driving/i_import_service.hpp"
#include "core/domain/ports/driven/i_json_validator.hpp"
#include "core/domain/ports/driven/i_request_sender.hpp"
#include "core/domain/ports/driven/i_token_provider.hpp"
#include "core/domain/ports/driven/i_variable_resolver.hpp"

namespace core {
class ThreadPool;
class StreamPool;
class CollectionStore;
class EnvironmentStore;
class SessionStore;
class AppConfigStore;
} // namespace core

namespace core::app {

class CoreApiClient final : public domain::IApiClient {
public:
  // Tunables are raw ints (0 -> default) so this public header stays free of the internal cache/ws structs.
  struct Config {
    std::string collectionRoot;   // collection dir; empty -> send-only
    std::string appConfigPath;    // empty -> OS app-support default
    core::AppConfig appDefaults;  // app-config defaults from .env
    // New-request defaults from .env (Core never reads .env itself; 0 -> Core built-in 30-min timeout):
    long long defaultTimeoutMs = 0; // DEFAULT_TIMEOUT_MS
    bool defaultVerifyTls = true;   // VERIFY_TLS
    // Response-cache ceilings/floor from .env (0 -> default):
    int ramCacheMaxMb = 0, ramCacheMinMb = 0, diskCacheMaxMb = 0, diskCacheMinMb = 0, ramCacheThresholdKb = 0;
    // WebSocket tunables from .env (0 -> default):
    int wsPingIntervalMs = 0, wsIdleTimeoutMs = 0, wsCloseTimeoutMs = 0, wsConnectTimeoutMs = 0,
        wsMaxFrameMb = 0, wsSendQueueMaxFrames = 0, wsSendQueueMaxMb = 0;
    // gRPC streaming ceilings from .env (0 -> default):
    long long streamMaxEvents = 0; // STREAM_MAX_EVENTS
    int streamMaxBytesMb = 0;      // STREAM_MAX_BYTES_MB
  };

  static std::unique_ptr<CoreApiClient> create(Config cfg);
  // Send-only client (no stores/cache) — for the CLI / e2e that only send.
  static std::unique_ptr<CoreApiClient> create();
  ~CoreApiClient() override; // defined in composition_root.cpp (incomplete ThreadPool member)

  domain::Result<domain::RequestExecutionId>
  send(const domain::RequestModel &request, std::shared_ptr<domain::IRequestObserver> observer) override;
  domain::Status cancel(domain::RequestExecutionId exec) override;
  domain::Status cancelAll() override;
  domain::Status sendStreamMessage(domain::RequestExecutionId exec, domain::WsMessage msg) override;
  domain::Status halfClose(domain::RequestExecutionId exec) override;
  domain::Status closeStream(domain::RequestExecutionId exec, int code, std::string reason) override;
  domain::Status validateJson(const domain::JsonText &) override;
  domain::Result<std::vector<domain::GrpcMethodDescriptor>>
  listGrpcMethods(const domain::GrpcRequest &) override;
  domain::Result<domain::GqlSchema> introspectGraphQl(const domain::RequestModel &) override;

  void setVariableScope(domain::VariableScope scope);
  // Pull merged vars (Global <- active env) into the send scope; cached.
  void refreshVariableScope();
  // True if a var in the send scope is still ciphertext -> the configured encryption key can't read it.
  bool hasUnreadableVars() const;

  // aliasifyModel rewrites literals back to {{alias}}; interactionOf classifies unary vs server-stream/bidi/duplex.
  std::string exportCurl(const core::domain::RequestModel &) const;
  core::domain::RequestModel aliasifyModel(const core::domain::RequestModel &) const;
  core::InteractionKind interactionOf(const core::domain::RequestModel &) const;
  // Resolve {{var}} in an arbitrary template against the active env.
  std::string resolvePreview(const std::string &tpl) const;

  // Pure (no network): classify pasted text, then parse it into a domain request.
  std::optional<domain::ImportKind> detectImport(const std::string &text) const;
  domain::Result<domain::ImportOutcome> importText(const std::string &text,
                                                   domain::ImportKind kind) const;

  // Valid only when created with a collection root; send-only clients have none.
  ICollectionRepository &collection() const;
  IEnvironmentRepository &environments() const;
  ISessionRepository &session() const;
  IAppConfigRepository &appConfig() const;
  IResponseCacheRepository &cache() const;

private:
  CoreApiClient() = default;

  // Owned stores: declared FIRST -> destroyed LAST, after the repos that reference them.
  std::unique_ptr<core::CollectionStore> ownCollection_;
  std::unique_ptr<core::EnvironmentStore> ownEnv_;
  std::unique_ptr<core::SessionStore> ownSession_;
  std::unique_ptr<core::AppConfigStore> ownAppConfig_;

  // Order matters: orchestrator_ is declared LAST so it is destroyed FIRST (may reference senders/clock/validator while sagas finish).
  std::vector<std::unique_ptr<domain::IRequestSender>> senders_;
  std::unique_ptr<domain::IClock> clock_;
  std::unique_ptr<domain::IJsonValidator> validator_;
  std::unique_ptr<domain::ITokenProvider> tokenProvider_; // OAuth2 (may be referenced by running sagas)
  std::unique_ptr<domain::IVariableResolver> resolver_;
  std::unique_ptr<domain::IImportService> importer_; // pasted-command import (curl/grpcurl/graphql)
  std::unique_ptr<ICollectionRepository> collectionRepo_;
  std::unique_ptr<IEnvironmentRepository> envRepo_;
  std::unique_ptr<ISessionRepository> sessionRepo_;
  std::unique_ptr<IAppConfigRepository> appConfigRepo_;
  std::unique_ptr<IResponseCacheRepository> cacheRepo_;
  domain::VariableScope scope_; // active env bindings ({{var}} resolution happens at send())
  // Merged vars (Global <- active), cached per (epoch, activeEnv); env edit bumps epoch -> invalidate.
  struct VarsSnapshot {
    std::uint64_t epoch = 0;
    bool undecryptable = false;   // some value stayed ciphertext (wrong/missing encryption key)
    std::string activeEnv;
    std::unordered_map<std::string, std::string> map;         // resolver view (active wins per key)
    std::vector<std::pair<std::string, std::string>> ordered; // aliasify view (active first, then non-shadowed Global)
  };
  std::shared_ptr<const VarsSnapshot> mergedVars() const;
  mutable std::mutex varsMu_;
  mutable std::shared_ptr<const VarsSnapshot> varsCache_;
  std::unique_ptr<RequestOrchestrator> orchestrator_;
  // Declared LAST -> destroyed FIRST: joins in-flight send tasks before orchestrator_/senders_ are torn down.
  // pool_ = bounded pool for unary sends; streamPool_ = one thread per long-lived stream so an indefinite stream never blocks unary sends.
  std::unique_ptr<core::ThreadPool> pool_;
  std::unique_ptr<core::StreamPool> streamPool_;
};

} // namespace core::app
