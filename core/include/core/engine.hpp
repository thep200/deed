// core/engine.hpp — Port VÀO (UI → Core). README §2 / UI spec §2.1.
// Engine dispatch theo request.type qua SenderRegistry; resolve {{var}} theo env active.
#pragma once

#include <map>
#include <memory>
#include <string>

#include "core/i_ui_delegate.hpp"
#include "core/importer.hpp"
#include "core/stores.hpp"
#include "core/types.hpp"

namespace core {

class SenderRegistry; // fwd (internal)

// Cấu hình khởi tạo Engine: thư mục collection + (tuỳ chọn) đường app-config.
struct EngineConfig {
    std::string collectionRoot;
    std::string appConfigPath; // rỗng -> dùng OS app-support mặc định
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
    SecretStore& secrets();
    AppConfigStore& appConfig();

    // --- Gửi (async, trả handle ngay) ---
    RequestHandle send(const RequestModel& model, IUiDelegate* delegate);
    void cancel(RequestHandle);

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

    // Build map biến hiệu lực (Global <- active env), secret lấy qua SecretStore.
    std::map<std::string, std::string> activeVars() const;

    // Resolve toàn bộ model -> ResolvedRequest (áp env + merge settings precedence + auth).
    ResolvedRequest resolveRequest(const RequestModel& model) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
