// core/engine.hpp — INBOUND port (UI → Core). README §2 / UI spec §2.1.
// Engine dispatches by request.type via SenderRegistry; resolves {{var}} per active env.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <optional>

#include "core/cache.hpp"
#include "core/i_ui_delegate.hpp"
#include "core/import_export/importer.hpp"
#include "core/persistence/stores.hpp"
#include "core/types.hpp"

namespace core {

class SenderRegistry; // fwd (internal)

// Cache ceiling/floor at ENV layer (ops-level). 0 = "unset" -> Core falls back to getenv/default.
// UI loads from .env file (DeedConfig) then passes in; Core does not read .env itself (stays pure C++).
struct CacheLimits {
    int ramMaxMb = 0;
    int ramMinMb = 0;
    int diskMaxMb = 0;
    int diskMinMb = 0;
    int thresholdKb = 0;
};

// Engine init config: collection dir + (optional) app-config path.
struct EngineConfig {
    std::string collectionRoot;
    std::string appConfigPath; // empty -> use OS app-support default
    CacheLimits cacheLimits;   // cache ceiling/floor from .env (empty -> getenv/default)
    AppConfig appDefaults;     // app-config default values from .env (when config.json misses a key)
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

    // Resolve whole model -> ResolvedRequest (apply env + merge settings precedence + auth).
    ResolvedRequest resolveRequest(const RequestModel& model) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
