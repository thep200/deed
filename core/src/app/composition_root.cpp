// core/src/app/composition_root.cpp — THE single place that wires concrete infra into the app
// (REFACTOR_SPEC §2). Only this TU (not the app headers) names the concrete senders/clock/validator, so
// the domain-purity gate stays green for everything under core/include.
#include "core/app/core_api_client.hpp"

#include "infra/import/import_service.hpp"
#include "infra/platform/json_validator.hpp"
#include "infra/platform/system_clock.hpp"
#include "infra/transport/graphql/native_graphql_sender.hpp"
#include "infra/transport/grpc/native_grpc_sender.hpp"
#include "infra/transport/http/native_http_sender.hpp"
#include "infra/transport/kafka/kafka_sender.hpp"
#include "infra/transport/ws/ws_sender.hpp"
#include "app/cache_config.hpp"     // detail::buildCacheConfig (native response cache)
#include "core/infra/variables/variable_resolver.hpp" // valueToAlias/prefixToAlias (native aliasify)
#include "core/infra/export/exporter.hpp" // core::toCurl (domain copy-as-cURL export)
#include "core/domain/graphql/gql_operation.hpp"    // domain effectiveOperation (native interactionOf)
#include "infra/platform/fs_util.hpp"        // fsutil::join (.session dir for the cache)
#include "infra/platform/stream_pool.hpp"
#include "infra/platform/thread_pool.hpp"
#include "infra/variables/domain_variable_resolver.hpp"
#include "infra/transport/graphql/gql_introspection.hpp" // native introspection (introspectGraphQl)
#include "infra/transport/grpc/grpc_method_listing.hpp" // native gRPC reflection (listGrpcMethods)

#include <chrono>
#include <functional>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace core::app {
namespace d = core::domain;

namespace {
// Native response cache (no Engine): owns the ResponseCache, replicating engine_cache.cpp exactly (revision
// fingerprint, ResponseRecord build, reload = recreate-on-enabled/persist-change). Shares the cache-config
// builder via cache_config.hpp so the effective config matches the Engine's byte-for-byte.
std::int64_t nowEpochMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
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
    // The domain ApiResponse carries no resolved-request dump, so there's no fingerprint to stamp; the
    // "stale response" badge was already inert on the live stack. Leave requestRevision empty.
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

// --- Native variable/classification helpers (no Engine): read the active env from the repos and reuse the
// pure resolver/alias/classification functions, mirroring Engine::activeVars/resolveRequest/aliasify/interactionOf.

// Active env vars (map). Active env name from session; keys from the env store. No "Global" base — matches
// Engine::activeVarsSnapshot (vars come only from the active env). Empty on any miss.
std::unordered_map<std::string, std::string> activeVarsMap(ISessionRepository *sess,
                                                          IEnvironmentRepository *env) {
  std::unordered_map<std::string, std::string> out;
  if (!sess || !env) return out;
  std::string active = sess->getActiveEnv();
  if (active.empty()) return out;
  try {
    core::Environment e = env->load(active);
    for (const auto &k : e.keys)
      if (k.enabled) out[k.key] = k.value;
  } catch (...) {
  }
  return out;
}
// Same in env-definition order (aliasify: first-defined key wins on duplicate values).
std::vector<std::pair<std::string, std::string>> activeVarsOrdered(ISessionRepository *sess,
                                                                   IEnvironmentRepository *env) {
  std::vector<std::pair<std::string, std::string>> out;
  if (!sess || !env) return out;
  std::string active = sess->getActiveEnv();
  if (active.empty()) return out;
  try {
    core::Environment e = env->load(active);
    for (const auto &k : e.keys)
      if (k.enabled) out.emplace_back(k.key, k.value);
  } catch (...) {
  }
  return out;
}
// Rebuild a domain GrpcRequest with its {{var}}-resolved target (immutable VO -> via Parts). Used by gRPC
// reflection (resolve the host typed with {{vars}} before the descriptor pass).
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
// Concrete app-layer collection repository over CollectionStore. The store speaks the DOMAIN RequestModel
// natively now, so load/save/create-from-model forward it directly (no bridge). The rest forward the
// surviving TreeNode/RequestType ops.
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
  std::string findRelPathById(const std::string &id) const override { return s_.findRelPathById(id); }
  int migrateAddIdToFilenames() const override { return s_.migrateAddIdToFilenames(); }

private:
  core::CollectionStore &s_;
};

// .env -> WsConfig / GrpcStreamLimits / CacheLimits builders (value-returning; keep create() under the size threshold).
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
    c->ownCollection_->setRequestDefaults(cfg.defaultTimeoutMs, cfg.defaultVerifyTls); // new-request .env defaults
    c->ownEnv_->migrateLegacySecrets(); // SPEC §T5 (Engine ctor did this)
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

  std::vector<d::IRequestSender *> ptrs;
  for (auto &s : c->senders_) ptrs.push_back(s.get());
  // Two pools (tech-debt fix): a long-lived stream (WS/gRPC-stream/Kafka-consumer) used to share this SAME
  // bounded pool with unary sends — enough concurrent streams could occupy every worker indefinitely and
  // starve new unary requests behind them. RequestOrchestrator now classifies each send and routes
  // accordingly; `pool` stays small/bounded (unary only), `streamPool` gives every stream its own thread.
  c->pool_ = std::make_unique<core::ThreadPool>();
  c->streamPool_ = std::make_unique<core::StreamPool>();
  core::ThreadPool *pool = c->pool_.get();
  core::StreamPool *streamPool = c->streamPool_.get();
  SendRequestSaga::Deps deps{ptrs, c->clock_.get(), c->validator_.get(), nullptr};
  c->orchestrator_ = std::make_unique<RequestOrchestrator>(
      deps,
      [pool](std::function<void()> job) {
        // submit() takes the task BY VALUE -> pass a copy so `job` stays valid for the inline fallback
        // when the pool rejects it (full/stopped). Moving here would leave `job` empty -> bad_function_call.
        if (!pool->submit(job)) job();
      },
      [streamPool](std::function<void()> job) {
        if (!streamPool->submit(job)) job();
      });
  return c;
}

void CoreApiClient::setVariableScope(d::VariableScope scope) { scope_ = std::move(scope); }

void CoreApiClient::refreshVariableScope() {
  d::VariableScope scope;
  scope.values = activeVarsMap(sessionRepo_.get(), envRepo_.get()); // native: reads the env repo, no Engine
  scope_ = std::move(scope);
}

std::string CoreApiClient::exportCurl(const core::domain::RequestModel &m) const {
  // Resolve {{var}} on the domain model against the active env, then render the cURL/grpcurl command.
  // toCurl reads config().tlsEnabledDefault for the gRPC -plaintext decision, so no legacy bridge or
  // per-request-config replication is needed — domain end to end. No public ResolvedRequest type leaks out.
  d::VariableScope scope;
  scope.values = activeVarsMap(sessionRepo_.get(), envRepo_.get());
  auto r = resolver_->resolve(m, scope);
  return core::toCurl(r.isOk() ? r.value() : m);
}

core::domain::RequestModel CoreApiClient::aliasifyModel(const core::domain::RequestModel &model) const {
  // Aliasify (literal -> {{alias}}) NATIVELY on the domain model: ordered active-env vars + the pure
  // VariableResolver matchers (prefix for url/target, whole-value for enabled kv/auth values). Rebuilds the
  // immutable VOs; a factory that would reject an aliased value falls back to the original. No bridge, no Engine.
  // Mirrors DomainVariableResolver's walk but the INVERSE op, and only over the fields the legacy aliasify
  // touched (url/target + kv + auth values — never body/query/message/onOpenSend).
  auto vars = activeVarsOrdered(sessionRepo_.get(), envRepo_.get());
  auto whole = [&](const std::string &s) -> std::string {
    std::string out, key;
    return core::VariableResolver::valueToAlias(s, vars, out, &key) ? out : s;
  };
  auto prefix = [&](const std::string &s) -> std::string {
    std::string out, key;
    return core::VariableResolver::prefixToAlias(s, vars, out, &key) ? out : s;
  };
  auto aliasUrl = [&](const d::Url &u) { return d::Url::create(prefix(u.raw())).take(); };
  auto aliasHeaders = [&](const d::HeaderList &hl) {
    std::vector<d::Header> out;
    out.reserve(hl.items().size());
    for (const auto &h : hl.items()) {
      auto r = d::Header::create(h.name(), h.enabled() ? whole(h.value()) : h.value(), h.enabled());
      out.push_back(r ? r.take() : h);
    }
    return d::HeaderList(std::move(out));
  };
  auto aliasParams = [&](const d::QueryParamList &pl) {
    std::vector<d::QueryParam> out;
    out.reserve(pl.items().size());
    for (const auto &p : pl.items()) {
      auto r = d::QueryParam::create(p.key(), p.enabled() ? whole(p.value()) : p.value(), p.enabled());
      out.push_back(r ? r.take() : p);
    }
    return d::QueryParamList(std::move(out));
  };
  auto aliasPathVars = [&](const d::PathVariableList &pl) {
    std::vector<d::PathVariable> out;
    out.reserve(pl.items().size());
    for (const auto &p : pl.items()) {
      auto r = d::PathVariable::create(p.key(), p.enabled() ? whole(p.value()) : p.value(), p.enabled());
      out.push_back(r ? r.take() : p);
    }
    return d::PathVariableList(std::move(out));
  };
  auto aliasMetadata = [&](const d::GrpcMetadata &md) {
    std::vector<d::MetadataEntry> out;
    out.reserve(md.entries().size());
    for (const auto &e : md.entries()) out.push_back({e.key, e.enabled ? whole(e.value) : e.value, e.enabled});
    auto r = d::GrpcMetadata::create(std::move(out));
    return r ? r.take() : md;
  };
  auto aliasAuth = [&](const d::Auth &a) -> d::Auth {
    return a.match([&](auto &&x) -> d::Auth {
      using T = std::decay_t<decltype(x)>;
      if constexpr (std::is_same_v<T, d::AuthNone>) return d::Auth::none();
      else if constexpr (std::is_same_v<T, d::AuthBearer>) {
        auto r = d::Auth::bearer(whole(x.token)); return r ? r.take() : a;
      } else { // AuthBasic
        auto r = d::Auth::basic(whole(x.username), whole(x.password)); return r ? r.take() : a;
      }
    });
  };
  auto payload = model.match([&](auto &&p) -> d::RequestModel::Payload {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, d::HttpRequest>) {
      d::HttpRequest::Parts hp{p.method(), aliasUrl(p.url()), aliasPathVars(p.pathVariables()),
                               aliasParams(p.params()), aliasHeaders(p.headers()), p.body(),
                               aliasAuth(p.auth())};
      return d::HttpRequest::create(std::move(hp)).take();
    } else if constexpr (std::is_same_v<T, d::GrpcRequest>) {
      d::GrpcRequest::Parts gp{prefix(p.target()), p.service(),         p.method(),
                               p.methodType(),     p.message(),         aliasMetadata(p.metadata()),
                               p.protoSource(),    p.tls()};
      return d::GrpcRequest::create(std::move(gp)).take();
    } else if constexpr (std::is_same_v<T, d::WebSocketRequest>) {
      d::WebSocketRequest::Parts wp{aliasUrl(p.url()), p.subprotocols(), aliasHeaders(p.headers()),
                                    aliasAuth(p.auth()), p.onOpenSend(), p.defaultSendKind()};
      auto r = d::WebSocketRequest::create(std::move(wp));
      return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
    } else if constexpr (std::is_same_v<T, d::KafkaRequest>) {
      // Alias brokers (prefix, like url/target) + topic/group (whole); message VALUE is body-like -> untouched.
      auto aliasTopic = [&](const d::KafkaTopic &t) {
        auto r = d::KafkaTopic::create(whole(t.value()));
        return r ? r.take() : t;
      };
      auto aliasKafkaHeaders = [&](const std::vector<d::KafkaHeader> &hs) {
        std::vector<d::KafkaHeader> out;
        out.reserve(hs.size());
        for (const auto &h : hs) out.push_back({h.key, h.enabled ? whole(h.value) : h.value, h.enabled});
        return out;
      };
      auto brokers = d::BrokerList::parse(prefix(p.brokers().toBootstrapServers()));
      d::BrokerList newBrokers = brokers ? brokers.take() : p.brokers();
      auto mode = p.match([&](auto &&spec) -> d::KafkaRequest::Mode {
        using S = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<S, d::KafkaProduceSpec>) {
          d::KafkaProduceSpec out = spec;
          out.config.topic = aliasTopic(spec.config.topic);
          if (out.message.key) out.message.key = d::MessageKey{whole(spec.message.key->value)};
          out.message.headers = aliasKafkaHeaders(spec.message.headers);
          return out;
        } else {
          d::KafkaConsumeSpec out = spec;
          std::vector<d::KafkaTopic> topics;
          for (const auto &t : spec.config.topics) topics.push_back(aliasTopic(t));
          out.config.topics = std::move(topics);
          auto g = d::ConsumerGroup::create(whole(spec.config.group.value()));
          out.config.group = g ? g.take() : spec.config.group;
          return out;
        }
      });
      auto r = d::KafkaRequest::create(newBrokers, p.security(), std::move(mode));
      return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
    } else { // GraphQlRequest — alias url + headers + auth only (query/variables untouched)
      d::GraphQlRequest::Parts gp{aliasUrl(p.url()), p.op(), aliasHeaders(p.headers()), aliasAuth(p.auth()),
                                  p.subTransport(), p.wsProtocol()};
      auto r = d::GraphQlRequest::create(std::move(gp));
      return r ? d::RequestModel::Payload(r.take()) : d::RequestModel::Payload(p);
    }
  });
  auto rebuilt =
      d::RequestModel::create(model.id(), model.name(), model.seq(), model.config(), std::move(payload));
  return rebuilt.isOk() ? rebuilt.take() : model;
}

std::string CoreApiClient::resolvePreview(const std::string &tpl) const {
  auto um = activeVarsMap(sessionRepo_.get(), envRepo_.get());
  std::map<std::string, std::string> vm(um.begin(), um.end());
  return core::VariableResolver::resolve(tpl, vm).text;
}

core::InteractionKind CoreApiClient::interactionOf(const core::domain::RequestModel &m) const {
  // Native classification on the DOMAIN model (pure: gql operation / HTTP-SSE via Accept header / gRPC
  // methodType). InteractionKind survives types.hpp (relocated to stream_events.hpp).
  switch (m.type()) {
  case d::RequestType::WebSocket:
    return core::InteractionKind::Duplex;
  case d::RequestType::GraphQl: {
    const auto &g = std::get<d::GraphQlRequest>(m.payload());
    return d::effectiveOperation(g) == d::GqlOperationType::Subscription
               ? core::InteractionKind::ServerStream
               : core::InteractionKind::Unary;
  }
  case d::RequestType::Http:
    // SSE = an enabled Accept: text/event-stream header (the native HTTP sender forces SSE on this signal).
    return d::acceptsEventStream(std::get<d::HttpRequest>(m.payload()))
               ? core::InteractionKind::ServerStream
               : core::InteractionKind::Unary;
  case d::RequestType::Grpc: {
    switch (std::get<d::GrpcRequest>(m.payload()).methodType()) {
    case d::GrpcMethodType::ServerStreaming: return core::InteractionKind::ServerStream;
    case d::GrpcMethodType::ClientStreaming: return core::InteractionKind::ClientStream;
    case d::GrpcMethodType::BidiStreaming: return core::InteractionKind::BiDi;
    default: return core::InteractionKind::Unary;
    }
  }
  case d::RequestType::Kafka: {
    // Producer = unary (one delivery report); Consumer = server-stream (inbound-only records, no push —
    // unlike WS's Duplex, cancel()/Stop is the only client->server signal, same shape as gRPC ServerStream).
    const auto &k = std::get<d::KafkaRequest>(m.payload());
    return k.kind() == d::KafkaClientKind::Consumer ? core::InteractionKind::ServerStream
                                                     : core::InteractionKind::Unary;
  }
  }
  return core::InteractionKind::Unary;
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
  // Resolve {{vars}} at the boundary (mirrors Engine's resolve-before-send). On resolver failure, fall
  // back to the unresolved request rather than dropping the send.
  if (resolver_) {
    auto resolved = resolver_->resolve(request, scope_);
    if (resolved.isOk()) return orchestrator_->send(resolved.value(), std::move(observer));
  }
  return orchestrator_->send(request, std::move(observer));
}
d::Status CoreApiClient::cancel(d::RequestExecutionId exec) { return orchestrator_->cancel(exec); }
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
  // Native reflection (no Engine): resolve {{var}} in the target, then run grpcdesc directly on domain types.
  auto resolved = resolveGrpcTarget(g, activeVarsMap(sessionRepo_.get(), envRepo_.get()));
  std::string err;
  std::vector<d::GrpcMethodDescriptor> descs = grpcdesc::listGrpcMethods(resolved, err);
  if (!err.empty())
    return d::Result<std::vector<d::GrpcMethodDescriptor>>::fail({d::ErrorCode::Network, err, ""});
  return d::Result<std::vector<d::GrpcMethodDescriptor>>::ok(std::move(descs));
}

d::Result<d::GqlSchema> CoreApiClient::introspectGraphQl(const d::RequestModel &request) {
  if (request.type() != d::RequestType::GraphQl)
    return d::Result<d::GqlSchema>::fail({d::ErrorCode::Validation, "not a graphql request", ""});
  // Resolve {{vars}} against the CURRENT active env (fresh read, like listGrpcMethods — the UI may call
  // this without a preceding refreshVariableScope). Resolver failure -> proceed unresolved, like send().
  d::VariableScope scope;
  scope.values = activeVarsMap(sessionRepo_.get(), envRepo_.get());
  d::RequestModel resolved = request;
  if (resolver_) {
    auto r = resolver_->resolve(request, scope);
    if (r.isOk()) resolved = r.take();
  }
  return gql::runIntrospection(resolved);
}

} // namespace core::app
