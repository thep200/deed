// core/engine.hpp — INBOUND port (UI → Core). README §2 / UI spec §2.1.
// Engine dispatches by request.type via SenderRegistry; resolves {{var}} per active env.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <optional>

#include "core/cache.hpp"
#include "core/i_ui_delegate.hpp"
#include "core/import_export/importer.hpp"
#include "core/persistence/stores.hpp"
#include "core/types.hpp"

namespace core {

class SenderRegistry; // fwd (internal)

// Opaque handle to track/cancel an in-flight stream (SPEC_grpc_streaming §4). Empty -> no stream.
struct StreamHandle { std::string streamId; };

// Cache ceiling/floor at ENV layer (ops-level). 0 = "unset" -> Core falls back to getenv/default.
// UI loads from .env file (DeedConfig) then passes in; Core does not read .env itself (stays pure C++).
struct CacheLimits {
    int ramMaxMb = 0;
    int ramMinMb = 0;
    int diskMaxMb = 0;
    int diskMinMb = 0;
    int thresholdKb = 0;
};

// Streaming ceilings from .env (SPEC_grpc_streaming §9). 0 = unset -> sender falls back to its default.
struct StreamLimits {
    std::uint64_t maxEvents = 0;   // truncate after this many events
    std::uint64_t maxBytes = 0;    // truncate after this many accumulated bytes
};

// Engine init config: collection dir + (optional) app-config path.
struct EngineConfig {
    std::string collectionRoot;
    std::string appConfigPath; // empty -> use OS app-support default
    CacheLimits cacheLimits;   // cache ceiling/floor from .env (empty -> getenv/default)
    AppConfig appDefaults;     // app-config default values from .env (when config.json misses a key)
    StreamLimits streamLimits; // stream ceilings from .env (empty -> sender default)
};

class Engine {
public:
    explicit Engine(EngineConfig config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // --- Lifecycle / collection ---
    void openCollection(const std::string& root);
    CollectionStore& collection();
    SessionStore& session();
    EnvironmentStore& environments();
    AppConfigStore& appConfig();

    // --- Send (async, returns handle immediately) ---
    RequestHandle send(const RequestModel& model, IUiDelegate* delegate);
    void cancel(RequestHandle);

    // --- Server-streaming (SPEC_grpc_streaming §4) ---
    // UI chooses the path: interactionOf(model)==ServerStream ? openStream() : send().
    // For gRPC this is derived from the method descriptor loaded when the RPC was picked (no round-trip).
    InteractionKind interactionOf(const RequestModel& model) const;

    // Open a stream; `sink` receives callbacks on a background thread (sink marshals to its UI thread).
    // Returns an opaque handle to cancel. Empty streamId means the stream was not started.
    StreamHandle openStream(const RequestModel& model, IStreamSink* sink);

    // Cancel a running stream (idempotent; unknown/empty handle -> no-op).
    void cancelStream(const StreamHandle& h);

    // --- Response cache (RESPONSE_CACHE.md). No-op when cache off (cacheResponses=false). ---
    // Store latest response/error for request id (overwrites old). resolvedRequestDump lives in resp.
    void putResponse(const std::string& id, const ApiResponse& resp);
    void putError(const std::string& id, const ApiError& err);
    // Get latest response (RAM first -> disk). nullopt if miss/cache off.
    std::optional<ResponseRecord> getResponse(const std::string& id);
    void removeResponse(const std::string& id);          // remove when request deleted
    void reloadCacheConfig();                            // call after Settings change
    const CacheConfig& cacheConfig() const;              // current effective config
    ResponseCache* responseCache();                      // direct access (test/diagnostic), null if off

    // --- gRPC: list available service/methods (for RPC dropdown) ---
    // reflection: query server via ServerReflection; protoFiles/descriptorSet: parse source.
    // SYNCHRONOUS + network IO for reflection — UI should call on a background thread.
    std::vector<GrpcMethodInfo> listGrpcMethods(const GrpcRequest& grpc, std::string& error) const;

    // --- Import cURL / grpcurl (CURL_IMPORT.md) ---
    // Classify text pasted into URL box; then parse into RequestModel (does NOT write file).
    bool looksLikeCurl(const std::string& text) const;
    bool looksLikeGrpcurl(const std::string& text) const;
    ImportResult importFromCurl(const std::string& text) const;   // type=http
    ImportResult importFromGrpc(const std::string& text) const;   // type=grpc

    // --- Synchronous helpers for UI ---
    ValidationResult validateJson(const std::string& text) const;
    // Resolve {{var}} per active env (Global + active) for preview.
    std::string resolvePreview(const std::string& tpl) const;

    // Build effective var map (Global <- active env), values plaintext from EnvironmentStore.
    std::map<std::string, std::string> activeVars() const;

    // Same merge as activeVars but preserving ENV DEFINITION ORDER (Global keys first, then the
    // active env's; a key in both keeps its first position with the active value). Used by
    // aliasify so that when two keys share a value, the FIRST-defined key wins as the alias.
    std::vector<std::pair<std::string, std::string>> activeVarsOrdered() const;

    // Resolve whole model -> ResolvedRequest (apply env + merge settings precedence + auth).
    ResolvedRequest resolveRequest(const RequestModel& model) const;

    // Distinct {{alias}} references in the model's injectable fields (url, path vars, query,
    // headers, auth; gRPC target/metadata) that are NOT present in the active env — in first-seen
    // order. Body is excluded on purpose (JSON/GraphQL legitimately contain braces). Empty ->
    // every alias resolves. UI uses this to warn + block a send (README §9.5).
    std::vector<std::string> missingVars(const RequestModel& model) const;

    // Proactively rewrite literal values that match the active env back to {{alias}}: url/target
    // by longest-prefix, headers/query/auth/metadata by whole-value. Returns the rewritten model;
    // `applied` (optional) receives the distinct alias keys that were substituted in (sorted).
    // No match -> fields are left unchanged. Idempotent. Triggered on send + import (README §9.5).
    RequestModel aliasifyModel(const RequestModel& model,
                               std::vector<std::string>* applied = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
