// core/engine.hpp — Port VÀO (UI → Core). README §2 / UI spec §2.1.
// Engine dispatch theo request.type qua SenderRegistry; resolve {{var}} theo env active.
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

// Trần/sàn cache ở ENV layer (ops-level). 0 = "không set" -> Core fallback getenv/default.
// UI nạp từ file .env (DeedConfig) rồi truyền vào; Core không tự đọc .env (giữ thuần C++).
struct CacheLimits {
    int ramMaxMb = 0;
    int ramMinMb = 0;
    int diskMaxMb = 0;
    int diskMinMb = 0;
    int thresholdKb = 0;
};

// Cấu hình khởi tạo Engine: thư mục collection + (tuỳ chọn) đường app-config.
struct EngineConfig {
    std::string collectionRoot;
    std::string appConfigPath; // rỗng -> dùng OS app-support mặc định
    CacheLimits cacheLimits;   // trần/sàn cache từ .env (rỗng -> getenv/default)
    AppConfig appDefaults;     // giá trị mặc định app-config từ .env (khi config.json thiếu key)
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

    // --- Gửi (async, trả handle ngay) ---
    RequestHandle send(const RequestModel& model, IUiDelegate* delegate);
    void cancel(RequestHandle);

    // --- Response cache (RESPONSE_CACHE.md). No-op khi tắt cache (cacheResponses=false). ---
    // Lưu response/lỗi mới nhất của request id (ghi đè bản cũ). resolvedRequestDump nằm trong resp.
    void putResponse(const std::string& id, const ApiResponse& resp);
    void putError(const std::string& id, const ApiError& err);
    // Lấy response gần nhất (RAM trước -> disk). nullopt nếu miss/tắt cache.
    std::optional<ResponseRecord> getResponse(const std::string& id);
    void removeResponse(const std::string& id);          // xoá khi xoá request
    void reloadCacheConfig();                            // gọi sau khi đổi Settings
    const CacheConfig& cacheConfig() const;              // cấu hình hiệu lực hiện tại
    ResponseCache* responseCache();                      // truy cập trực tiếp (test/diagnostic), null nếu tắt

    // --- gRPC: liệt kê service/method khả dụng (cho dropdown chọn RPC) ---
    // reflection: query server qua ServerReflection; protoFiles/descriptorSet: parse nguồn.
    // ĐỒNG BỘ + có IO mạng cho reflection — UI nên gọi trên thread nền.
    std::vector<GrpcMethodInfo> listGrpcMethods(const GrpcRequest& grpc, std::string& error) const;

    // --- Import cURL / grpcurl (CURL_IMPORT.md) ---
    // Phân loại text dán vào ô URL; rồi parse sang RequestModel (KHÔNG ghi file).
    bool looksLikeCurl(const std::string& text) const;
    bool looksLikeGrpcurl(const std::string& text) const;
    ImportResult importFromCurl(const std::string& text) const;   // type=http
    ImportResult importFromGrpc(const std::string& text) const;   // type=grpc

    // --- Tiện ích đồng bộ cho UI ---
    ValidationResult validateJson(const std::string& text) const;
    // Resolve {{var}} theo env active (Global + active) để preview.
    std::string resolvePreview(const std::string& tpl) const;

    // Build map biến hiệu lực (Global <- active env), value plaintext từ EnvironmentStore.
    std::map<std::string, std::string> activeVars() const;

    // Resolve toàn bộ model -> ResolvedRequest (áp env + merge settings precedence + auth).
    ResolvedRequest resolveRequest(const RequestModel& model) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
