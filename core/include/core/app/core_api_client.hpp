// core/app/core_api_client.hpp — production IApiClient assembled at the composition root (REFACTOR_SPEC §2/§7.3).
// Owns the real senders + infra adapters + orchestrator and forwards the IApiClient surface to the
// orchestrator. The HEADER stays pure (domain ports + app orchestrator only); the concrete infra types
// (cpr/grpc senders, clock, json validator) are constructed in composition_root.cpp — the one place that
// knows them.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/app/persistence_repositories.hpp" // env/session/app-config repository ports
#include "core/streaming/stream_events.hpp"      // InteractionKind (send routing classification)
#include "core/app/request_orchestrator.hpp"
#include "core/domain/ports/i_api_client.hpp"
#include "core/domain/ports/i_clock.hpp"
#include "core/domain/ports/i_import_service.hpp"
#include "core/domain/ports/i_json_validator.hpp"
#include "core/domain/ports/i_request_sender.hpp"
#include "core/domain/ports/i_variable_resolver.hpp"

namespace core {
class ThreadPool;
class CollectionStore;
class EnvironmentStore;
class SessionStore;
class AppConfigStore;
} // namespace core

namespace core::app {

class CoreApiClient final : public domain::IApiClient {
public:
  // Self-sufficient configuration (REFACTOR_SPEC §2): CoreApiClient owns its own stores + response cache,
  // no Engine. `collectionRoot` empty -> a send-only client (no persistence repos / cache). The cache/.env
  // tunables are raw ints (0 -> default) so this public header stays free of the internal cache/ws structs.
  struct Config {
    std::string collectionRoot;   // collection dir; empty -> send-only
    std::string appConfigPath;    // empty -> OS app-support default
    core::AppConfig appDefaults;  // app-config defaults from .env (env_config.hpp)
    // Response-cache ceilings/floor from .env (0 -> default):
    int ramCacheMaxMb = 0, ramCacheMinMb = 0, diskCacheMaxMb = 0, diskCacheMinMb = 0, ramCacheThresholdKb = 0;
    // WebSocket tunables from .env (0 -> default):
    int wsPingIntervalMs = 0, wsIdleTimeoutMs = 0, wsCloseTimeoutMs = 0, wsMaxFrameMb = 0,
        wsSendQueueMaxFrames = 0, wsSendQueueMaxMb = 0;
  };

  // Composition root: build the full self-owned stack (senders + clock + validator + orchestrator + the
  // stores/cache when `collectionRoot` is set). No Engine.
  static std::unique_ptr<CoreApiClient> create(Config cfg);
  // Send-only client (no stores/cache) — for the CLI / e2e that only send.
  static std::unique_ptr<CoreApiClient> create();
  ~CoreApiClient() override; // defined in composition_root.cpp (incomplete ThreadPool member)

  domain::Result<domain::RequestExecutionId>
  send(const domain::RequestModel &request, std::shared_ptr<domain::IRequestObserver> observer) override;
  domain::Status cancel(domain::RequestExecutionId exec) override;
  domain::Status sendStreamMessage(domain::RequestExecutionId exec, domain::WsMessage msg) override;
  domain::Status halfClose(domain::RequestExecutionId exec) override;
  domain::Status closeStream(domain::RequestExecutionId exec, int code, std::string reason) override;
  domain::Status validateJson(const domain::JsonText &) override;
  domain::Result<std::vector<domain::GrpcMethodDescriptor>>
  listGrpcMethods(const domain::GrpcRequest &) override;

  // Set the active environment's {{var}} bindings used to resolve requests before sending. The UI calls
  // this when the active environment changes (mirrors Engine's active-env snapshot).
  void setVariableScope(domain::VariableScope scope);
  // Pull the active env's merged vars (Global <- active) from the Engine into the send scope. The UI calls
  // this right before a send so {{vars}} resolve against the current environment (replaces the UI reading
  // Engine::activeVars() directly). No-op without an Engine.
  void refreshVariableScope();

  // Request-services facade (variable resolution + RPC classification), domain-typed.
  // exportCurl resolves env + per-request config then renders a cURL/grpcurl command (legacy toCurl kept
  // inside the composition root); aliasifyModel rewrites literals back to {{alias}}; interactionOf classifies
  // unary vs server-stream/bidi/duplex.
  std::string exportCurl(const core::domain::RequestModel &) const;
  core::domain::RequestModel aliasifyModel(const core::domain::RequestModel &) const;
  core::InteractionKind interactionOf(const core::domain::RequestModel &) const;
  // Resolve {{var}} in an arbitrary template against the active env (CLI preview helper).
  std::string resolvePreview(const std::string &tpl) const;

  // Import use-cases (REFACTOR_SPEC §6.1 non-send use-cases). Classify pasted text, then parse it into a
  // domain request. Pure (no network) — always available regardless of the WS/reflection Engine.
  std::optional<domain::ImportKind> detectImport(const std::string &text) const;
  domain::Result<domain::ImportOutcome> importText(const std::string &text,
                                                   domain::ImportKind kind) const;

  // Persistence repositories (REFACTOR_SPEC §6.3/§8.3). Valid only when created with an Engine (the GUI
  // path); they wrap that Engine's stores and return the clean POD config types (env_config.hpp) — the
  // collection repo returns the legacy TreeNode/RequestModel the UI tree consumes (transitional).
  ICollectionRepository &collection() const;
  IEnvironmentRepository &environments() const;
  ISessionRepository &session() const;
  IAppConfigRepository &appConfig() const;
  IResponseCacheRepository &cache() const;

private:
  CoreApiClient() = default;

  // Owned stores (when created with a Config). Declared FIRST -> destroyed LAST, after the repos that
  // reference them. Held by unique_ptr so the header only forward-declares the concrete store classes.
  std::unique_ptr<core::CollectionStore> ownCollection_;
  std::unique_ptr<core::EnvironmentStore> ownEnv_;
  std::unique_ptr<core::SessionStore> ownSession_;
  std::unique_ptr<core::AppConfigStore> ownAppConfig_;

  // Declaration order matters: orchestrator_ is declared LAST so it is destroyed FIRST (it may reference
  // the senders/clock/validator while sagas finish).
  std::vector<std::unique_ptr<domain::IRequestSender>> senders_;
  std::unique_ptr<domain::IClock> clock_;
  std::unique_ptr<domain::IJsonValidator> validator_;
  std::unique_ptr<domain::IVariableResolver> resolver_;
  std::unique_ptr<domain::IImportService> importer_; // pasted-command import (curl/grpcurl/graphql)
  std::unique_ptr<ICollectionRepository> collectionRepo_; // collection/env/session/app-config/cache repos
  std::unique_ptr<IEnvironmentRepository> envRepo_;        // over the Engine stores/cache
  std::unique_ptr<ISessionRepository> sessionRepo_;
  std::unique_ptr<IAppConfigRepository> appConfigRepo_;
  std::unique_ptr<IResponseCacheRepository> cacheRepo_;
  domain::VariableScope scope_; // active env bindings ({{var}} resolution happens at send())
  std::unique_ptr<RequestOrchestrator> orchestrator_;
  // Declared LAST -> destroyed FIRST: joins in-flight send tasks (which reference the orchestrator/senders
  // above) before those are torn down.
  std::unique_ptr<core::ThreadPool> pool_;
};

} // namespace core::app
