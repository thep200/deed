// Only this TU names concrete senders/clock/validator so the domain-purity gate stays green.
#include "core/app/core_api_client.hpp"

#include "infra/import/import_service.hpp"
#include "infra/platform/json_validator.hpp"
#include "infra/platform/system_clock.hpp"
#include "infra/transport/graphql/native_graphql_sender.hpp"
#include "infra/transport/grpc/native_grpc_sender.hpp"
#include "infra/transport/http/native_http_sender.hpp"
#include "infra/transport/kafka/kafka_sender.hpp"
#include "infra/transport/ws/ws_sender.hpp"
#include "app/repo_adapters.hpp"
#include "core/infra/variables/variable_resolver.hpp"
#include "core/infra/export/exporter.hpp"
#include "core/domain/request/interaction_of.hpp"
#include "app/aliasify.hpp"
#include "infra/platform/fs_util.hpp"
#include "infra/platform/stream_pool.hpp"
#include "infra/platform/thread_pool.hpp"
#include "infra/variables/domain_variable_resolver.hpp"
#include "core/domain/auth/with_auth.hpp"
#include "infra/auth/oauth2_token_provider.hpp"
#include "infra/transport/graphql/gql_introspection.hpp"
#include "infra/transport/grpc/grpc_method_listing.hpp"
#include "infra/transport/soap/native_soap_sender.hpp"
#include "infra/transport/ldap/native_ldap_sender.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace core::app {
namespace d = core::domain;

namespace {
// Enabled keys of one env, definition order. Missing/corrupt file -> nothing.
void appendEnabledKeys(IEnvironmentRepository *env, const std::string &name, std::vector<core::EnvKey> &out) {
  try {
    core::Environment e = env->load(name);
    for (auto &k : e.keys)
      if (k.enabled) out.push_back(std::move(k));
  } catch (...) {
  }
}
// Global first, active wins per key; empty active value never shadows non-empty Global (grid saves "" for blanks).
void mergeVarsMap(const std::vector<core::EnvKey> &globalKeys, const std::vector<core::EnvKey> &activeKeys,
                  std::unordered_map<std::string, std::string> &map) {
  for (const auto &k : globalKeys) map[k.key] = k.value;
  for (const auto &k : activeKeys) {
    auto it = map.find(k.key);
    if (it == map.end()) map.emplace(k.key, k.value);
    else if (!k.value.empty()) it->second = k.value;
  }
}
// Ordered view (first-wins aliasify): active pairs first, then non-shadowed Global pairs.
void mergeVarsOrdered(const std::vector<core::EnvKey> &globalKeys, const std::vector<core::EnvKey> &activeKeys,
                      std::vector<std::pair<std::string, std::string>> &ordered) {
  ordered.reserve(activeKeys.size() + globalKeys.size());
  for (const auto &k : activeKeys) ordered.emplace_back(k.key, k.value);
  for (const auto &k : globalKeys) {
    bool shadowed = false;
    for (const auto &a : activeKeys)
      if (a.key == k.key && !a.value.empty()) { shadowed = true; break; }
    if (!shadowed) ordered.emplace_back(k.key, k.value);
  }
}
// Immutable VO -> rebuild via Parts with the {{var}}-resolved target.
core::domain::GrpcRequest resolveGrpcTarget(const core::domain::GrpcRequest &g,
                                            const std::unordered_map<std::string, std::string> &um) {
  std::map<std::string, std::string> vm(um.begin(), um.end());
  core::domain::GrpcRequest::Parts p;
  p.target = core::VariableResolver::resolve(g.target(), vm).text;
  p.service = g.service();
  p.method = g.method();
  p.methodType = g.methodType();
  p.message = g.message();
  p.metadata = g.metadata();
  p.protoSource = g.protoSource();
  p.tls = g.tls();
  return core::domain::GrpcRequest::create(std::move(p)).take();
}
core::WsConfig buildWsConfig(const CoreApiClient::Config &cfg) {
  core::WsConfig ws; // 0 -> ws_sender default
  if (cfg.wsPingIntervalMs > 0) ws.pingIntervalMs = cfg.wsPingIntervalMs;
  if (cfg.wsIdleTimeoutMs > 0) ws.idleTimeoutMs = cfg.wsIdleTimeoutMs;
  if (cfg.wsCloseTimeoutMs > 0) ws.closeTimeoutMs = cfg.wsCloseTimeoutMs;
  if (cfg.wsConnectTimeoutMs > 0) ws.connectTimeoutMs = cfg.wsConnectTimeoutMs;
  if (cfg.wsMaxFrameMb > 0) ws.maxFrameBytes = static_cast<std::uint64_t>(cfg.wsMaxFrameMb) * 1024 * 1024;
  if (cfg.wsSendQueueMaxFrames > 0) ws.sendQueueMaxFrames = static_cast<std::size_t>(cfg.wsSendQueueMaxFrames);
  if (cfg.wsSendQueueMaxMb > 0)
    ws.sendQueueMaxBytes = static_cast<std::uint64_t>(cfg.wsSendQueueMaxMb) * 1024 * 1024;
  return ws;
}
infra::GrpcStreamLimits buildGrpcStreamLimits(const CoreApiClient::Config &cfg) {
  infra::GrpcStreamLimits lim; // 0 -> grpc_sender default
  if (cfg.streamMaxEvents > 0) lim.maxEvents = static_cast<std::uint64_t>(cfg.streamMaxEvents);
  if (cfg.streamMaxBytesMb > 0) lim.maxBytes = static_cast<std::uint64_t>(cfg.streamMaxBytesMb) * 1024 * 1024;
  return lim;
}
core::CacheLimits buildCacheLimits(const CoreApiClient::Config &cfg) {
  core::CacheLimits limits;
  limits.ramMaxMb = cfg.ramCacheMaxMb;
  limits.ramMinMb = cfg.ramCacheMinMb;
  limits.diskMaxMb = cfg.diskCacheMaxMb;
  limits.diskMinMb = cfg.diskCacheMinMb;
  limits.thresholdKb = cfg.ramCacheThresholdKb;
  return limits;
}
} // namespace

CoreApiClient::~CoreApiClient() = default; // ThreadPool complete here -> can destroy the unique_ptr member

std::unique_ptr<CoreApiClient> CoreApiClient::create() { return create(Config{}); }

std::unique_ptr<CoreApiClient> CoreApiClient::create(Config cfg) {
  std::unique_ptr<CoreApiClient> c(new CoreApiClient());
  c->clock_ = std::make_unique<infra::SystemClock>();
  c->validator_ = std::make_unique<infra::JsonValidator>();
  c->tokenProvider_ = std::make_unique<infra::oauth2::OAuth2TokenProvider>(c->clock_.get());
  c->resolver_ = std::make_unique<infra::DomainVariableResolver>();
  c->importer_ = std::make_unique<infra::ImportService>();

  // Own the stores + response cache (GUI path; CLI/tests with empty root are send-only).
  if (!cfg.collectionRoot.empty()) {
    c->ownCollection_ = std::make_unique<core::CollectionStore>(cfg.collectionRoot);
    c->ownEnv_ = std::make_unique<core::EnvironmentStore>(cfg.collectionRoot);
    c->ownSession_ = std::make_unique<core::SessionStore>(cfg.collectionRoot);
    c->ownAppConfig_ = cfg.appConfigPath.empty() ? std::make_unique<core::AppConfigStore>()
                                                 : std::make_unique<core::AppConfigStore>(cfg.appConfigPath);
    c->ownAppConfig_->setDefaults(cfg.appDefaults);
    c->ownEnv_->attachAppConfig(c->ownAppConfig_.get()); // encrypt-at-rest key/exclude source
    c->ownCollection_->setRequestDefaults(cfg.defaultTimeoutMs, cfg.defaultVerifyTls);
    c->ownCollection_->ensureGitignore();

    c->collectionRepo_ = std::make_unique<CollectionRepository>(*c->ownCollection_);
    c->envRepo_ = std::make_unique<EnvironmentRepository>(*c->ownEnv_);
    c->sessionRepo_ = std::make_unique<SessionRepository>(*c->ownSession_);
    c->appConfigRepo_ = std::make_unique<AppConfigRepository>(*c->ownAppConfig_);

    c->cacheRepo_ = std::make_unique<NativeResponseCacheRepository>(
        c->ownAppConfig_.get(), buildCacheLimits(cfg), core::fsutil::join(cfg.collectionRoot, ".session"));
  }

  c->senders_.push_back(std::make_unique<infra::NativeHttpSender>());
  c->senders_.push_back(std::make_unique<infra::NativeGrpcSender>(buildGrpcStreamLimits(cfg)));
  c->senders_.push_back(std::make_unique<infra::NativeGraphQlSender>());
  c->senders_.push_back(std::make_unique<infra::WsSenderAdapter>(buildWsConfig(cfg)));
  c->senders_.push_back(std::make_unique<infra::KafkaSender>());
  c->senders_.push_back(std::make_unique<infra::NativeSoapSender>());
  c->senders_.push_back(std::make_unique<infra::NativeLdapSender>());

  std::vector<d::IRequestSender *> ptrs;
  for (auto &s : c->senders_) ptrs.push_back(s.get());
  // Long-lived streams (WS/gRPC-stream/Kafka-consumer) get their own pool so they can't starve unary sends.
  c->pool_ = std::make_unique<core::ThreadPool>();
  c->streamPool_ = std::make_unique<core::StreamPool>();
  core::ThreadPool *pool = c->pool_.get();
  core::StreamPool *streamPool = c->streamPool_.get();
  SendRequestSaga::Deps deps{ptrs, c->clock_.get(), c->validator_.get(), nullptr,
                             c->tokenProvider_.get()};
  c->orchestrator_ = std::make_unique<RequestOrchestrator>(
      deps,
      [pool, streamPool](std::function<void()> job) {
        // submit() takes the task by value -> pass a copy so `job` stays valid for the next fallback.
        // streamPool before inline: inline runs the send on the UI thread, where a hang freezes Cancel.
        if (!pool->submit(job) && !streamPool->submit(job)) job();
      },
      [streamPool](std::function<void()> job) {
        if (!streamPool->submit(job)) job();
      });
  return c;
}

void CoreApiClient::setVariableScope(d::VariableScope scope) { scope_ = std::move(scope); }

std::shared_ptr<const CoreApiClient::VarsSnapshot> CoreApiClient::mergedVars() const {
  // Epoch read BEFORE load: racing edit -> next call rebuilds (never fresh-epoch/stale-data).
  const std::uint64_t epoch = ownEnv_ ? ownEnv_->epoch() : 0;
  const std::string active = sessionRepo_ ? sessionRepo_->getActiveEnv() : std::string();
  std::lock_guard<std::mutex> lk(varsMu_);
  if (varsCache_ && varsCache_->epoch == epoch && varsCache_->activeEnv == active) return varsCache_;
  auto snap = std::make_shared<VarsSnapshot>();
  snap->epoch = epoch;
  snap->activeEnv = active;
  if (envRepo_) {
    std::vector<core::EnvKey> globalKeys, activeKeys;
    appendEnabledKeys(envRepo_.get(), core::kGlobalEnvName, globalKeys);
    if (!active.empty() && active != core::kGlobalEnvName)
      appendEnabledKeys(envRepo_.get(), active, activeKeys);
    mergeVarsMap(globalKeys, activeKeys, snap->map);
    mergeVarsOrdered(globalKeys, activeKeys, snap->ordered);
    for (const auto &kv : snap->map)   // ciphertext left in scope -> the key can't read it
      if (core::EnvironmentStore::isEncryptedValue(kv.second)) { snap->undecryptable = true; break; }
  }
  varsCache_ = snap;
  return snap;
}

void CoreApiClient::refreshVariableScope() {
  d::VariableScope scope;
  scope.values = mergedVars()->map;
  scope_ = std::move(scope);
}

bool CoreApiClient::hasUnreadableVars() const { return mergedVars()->undecryptable; }

std::string CoreApiClient::exportCurl(const core::domain::RequestModel &m) const {
  d::VariableScope scope;
  scope.values = mergedVars()->map;
  auto r = resolver_->resolve(m, scope);
  return core::toCurl(r.isOk() ? r.value() : m);
}

core::domain::RequestModel CoreApiClient::aliasifyModel(const core::domain::RequestModel &model) const {
  return ::core::app::aliasifyModel(model, mergedVars()->ordered);
}

std::string CoreApiClient::resolvePreview(const std::string &tpl) const {
  auto snap = mergedVars();
  std::map<std::string, std::string> vm(snap->map.begin(), snap->map.end());
  return core::VariableResolver::resolve(tpl, vm).text;
}

core::InteractionKind CoreApiClient::interactionOf(const core::domain::RequestModel &m) const {
  return d::interactionOf(m);
}

std::optional<d::ImportKind> CoreApiClient::detectImport(const std::string &text) const {
  return importer_->detect(text);
}
d::Result<d::ImportOutcome> CoreApiClient::importText(const std::string &text, d::ImportKind kind) const {
  return importer_->import(text, kind);
}

ICollectionRepository &CoreApiClient::collection() const { return *collectionRepo_; }
IEnvironmentRepository &CoreApiClient::environments() const { return *envRepo_; }
ISessionRepository &CoreApiClient::session() const { return *sessionRepo_; }
IAppConfigRepository &CoreApiClient::appConfig() const { return *appConfigRepo_; }
IResponseCacheRepository &CoreApiClient::cache() const { return *cacheRepo_; }

d::Result<d::RequestExecutionId>
CoreApiClient::send(const d::RequestModel &request, std::shared_ptr<d::IRequestObserver> observer) {
  // Resolver failure -> send unresolved rather than dropping the send.
  if (resolver_) {
    auto resolved = resolver_->resolve(request, scope_);
    if (resolved.isOk()) return orchestrator_->send(resolved.value(), std::move(observer));
  }
  return orchestrator_->send(request, std::move(observer));
}
d::Status CoreApiClient::cancel(d::RequestExecutionId exec) { return orchestrator_->cancel(exec); }
d::Status CoreApiClient::cancelAll() { return orchestrator_->cancelAll(); }
d::Status CoreApiClient::sendStreamMessage(d::RequestExecutionId exec, d::WsMessage msg) {
  return orchestrator_->sendStreamMessage(exec, std::move(msg));
}
d::Status CoreApiClient::halfClose(d::RequestExecutionId exec) { return orchestrator_->halfClose(exec); }
d::Status CoreApiClient::closeStream(d::RequestExecutionId exec, int code, std::string reason) {
  return orchestrator_->closeStream(exec, code, std::move(reason));
}
d::Status CoreApiClient::validateJson(const d::JsonText &t) { return orchestrator_->validateJson(t); }

d::Result<std::vector<d::GrpcMethodDescriptor>>
CoreApiClient::listGrpcMethods(const d::GrpcRequest &g) {
  auto resolved = resolveGrpcTarget(g, mergedVars()->map);
  std::string err;
  std::vector<d::GrpcMethodDescriptor> descs = grpcdesc::listGrpcMethods(resolved, err);
  if (!err.empty())
    return d::Result<std::vector<d::GrpcMethodDescriptor>>::fail({d::ErrorCode::Network, err, ""});
  return d::Result<std::vector<d::GrpcMethodDescriptor>>::ok(std::move(descs));
}

d::Result<d::GqlSchema> CoreApiClient::introspectGraphQl(const d::RequestModel &request) {
  if (request.type() != d::RequestType::GraphQl)
    return d::Result<d::GqlSchema>::fail({d::ErrorCode::Validation, "not a graphql request", ""});
  // Fresh env read (UI may skip refreshVariableScope); resolver failure -> proceed unresolved.
  d::VariableScope scope;
  scope.values = mergedVars()->map;
  d::RequestModel resolved = request;
  if (resolver_) {
    auto r = resolver_->resolve(request, scope);
    if (r.isOk()) resolved = r.take();
  }
  // Introspection bypasses the saga, so OAuth2 materializes here too (Schema tab honors the Auth tab).
  if (const auto *oauth = d::oauth2Of(resolved)) {
    if (!tokenProvider_)
      return d::Result<d::GqlSchema>::fail({d::ErrorCode::Unsupported, "oauth2 not configured", ""});
    d::NoCancel cancel;
    auto tok = tokenProvider_->bearerFor(*oauth, resolved.config().timeout, cancel);
    if (!tok.isOk())
      return d::Result<d::GqlSchema>::fail(
          {tok.error().code, "oauth2 token: " + tok.error().message, ""});
    auto bearer = d::Auth::bearer(tok.take());
    if (!bearer.isOk()) return d::Result<d::GqlSchema>::fail(bearer.error());
    resolved = d::withAuth(resolved, bearer.take());
  }
  return gql::runIntrospection(resolved);
}

} // namespace core::app
