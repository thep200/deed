// core/types.hpp — DTO trung lập của Core (README §7).
// UI chỉ thấy các struct này; KHÔNG leak kiểu của grpc++/protobuf/libcurl/nlohmann ra port.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace core {

// ---- Phân loại giao thức (khớp trường "type" trong file request) ----
enum class RequestType { Http, Grpc };

std::string toString(RequestType);
bool parseRequestType(const std::string&, RequestType& out);

// ---- Dòng key/value (headers, params, metadata, pathVariables, form...) ----
// Mảng (không object) để giữ thứ tự, cho phép trùng key, mỗi dòng có enabled. (README §7.4)
struct KeyValue {
    std::string key;
    std::string value;
    bool enabled = true;
};

// ---- Body của HTTP (tagged union theo mode) ----
struct MultipartPart {
    std::string key;
    std::string value;      // dùng khi type == "text"
    std::string type;       // "text" | "file"
    std::string filePath;   // dùng khi type == "file"
    bool enabled = true;
};

struct Body {
    // none | json | text | xml | form-urlencoded | multipart | binary | graphql
    std::string mode = "none";
    std::string json;
    std::string text;
    std::string xml;
    std::vector<KeyValue> formUrlEncoded;
    std::vector<MultipartPart> multipart;
    std::string binaryFilePath;
    std::string graphqlQuery;
    std::string graphqlVariables;
};

// ---- Auth của HTTP ----
struct Auth {
    std::string type = "none";          // none | basic | bearer | apikey
    std::string basicUsername;
    std::string basicPassword;
    std::string bearerToken;
    std::string apikeyKey;
    std::string apikeyValue;
    std::string apikeyIn = "header";    // header | query
};

struct HttpSettings {
    int timeoutMs = 30000;
    bool followRedirects = true;
    bool verifyTls = true;
    bool timeoutMsSet = false;          // để merge precedence biết field có được set chưa
    bool followRedirectsSet = false;
    bool verifyTlsSet = false;
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<KeyValue> pathVariables;
    std::vector<KeyValue> params;
    std::vector<KeyValue> headers;
    Body body;
    Auth auth;
    HttpSettings settings;
};

// ---- gRPC ----
struct ProtoSource {
    // reflection | protoFiles | descriptorSet
    std::string mode = "reflection";
    std::vector<std::string> files;        // protoFiles
    std::vector<std::string> importPaths;  // protoFiles
    std::string descriptorSetPath;         // descriptorSet
};

struct GrpcTls {
    bool enabled = false;
    bool insecureSkipVerify = false;
    std::string caCertPath;
    std::string clientCertPath;
    std::string clientKeyPath;
};

struct GrpcSettings {
    int deadlineMs = 30000;
    bool deadlineMsSet = false;
};

// Một RPC khả dụng (để UI dựng dropdown chọn service/method).
struct GrpcMethodInfo {
    std::string service;     // pkg.Service đầy đủ
    std::string method;      // tên RPC
    std::string methodType;  // unary | server_streaming | client_streaming | bidi_streaming
};

struct GrpcRequest {
    std::string target;          // host:port (authority, không phải URL có path)
    std::string service;         // pkg.Service đầy đủ
    std::string method;          // tên RPC
    std::string methodType = "unary"; // unary | server_streaming | client_streaming | bidi_streaming
    ProtoSource protoSource;
    std::string message;         // JSON; Core marshal sang protobuf khi gửi
    std::vector<KeyValue> metadata;
    GrpcTls tls;
    GrpcSettings settings;
};

// ---- Mô hình một request (envelope + khối theo type) ----
struct RequestModel {
    int schemaVersion = 1;
    std::string id;
    std::string name;
    std::string description;
    RequestType type = RequestType::Http;
    int seq = 0;

    HttpRequest http;   // dùng khi type == Http
    GrpcRequest grpc;   // dùng khi type == Grpc
};

// ResolvedRequest: RequestModel sau khi resolve {{var}} + áp auth. (README §8.1)
// Dùng chung struct cho gọn — sender chỉ nhận bản đã resolve.
struct ResolvedRequest {
    RequestModel model;
};

// ---- Kết quả gọi ----
struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path;
    std::string expires;
};

struct ApiResponse {
    // HTTP
    int statusCode = 0;
    std::string statusText;
    std::vector<KeyValue> headers;
    std::vector<Cookie> cookies;     // Set-Cookie của response hiện tại (POC, không jar)
    // chung
    std::string body;                // HTTP body hoặc gRPC message JSON
    long elapsedMs = 0;
    std::int64_t sizeBytes = 0;
    std::string resolvedRequestDump; // request đã resolve (tab Request để debug)
};

enum class ErrorKind { Network, Timeout, Tls, Cancelled, Parse, Unsupported, Unknown };
std::string toString(ErrorKind);

struct ApiError {
    ErrorKind kind = ErrorKind::Unknown;
    std::string message;
};

// ---- Validate JSON nhẹ (README §7 của UI spec) ----
struct ValidationResult {
    bool ok = true;
    int line = 0;
    int col = 0;
    std::string msg;
};

// ---- Cây collection (lazy, chỉ metadata) ----
struct TreeNode {
    std::string name;       // tên hiển thị (request: field name; folder: tên thư mục)
    std::string relPath;    // đường dẫn tương đối so với gốc collection
    bool isFolder = false;
    // chỉ có khi !isFolder:
    std::string id;         // id ổn định của request (sống sót qua rename/move)
    RequestType requestType = RequestType::Http;
    std::string methodOrType; // HTTP method, hoặc gRPC methodType, để hiển thị badge
    std::vector<TreeNode> children;
};

// ---- Environment ----
struct EnvKey {
    std::string key;
    std::string value;      // rỗng nếu secret (giá trị nằm ở SecretStore)
    bool secret = false;
    bool enabled = true;
};

struct Environment {
    std::string name;
    int schemaVersion = 1;
    std::vector<EnvKey> keys;
};

// ---- App-global config (README §12.1) ----
struct AppConfig {
    int defaultTimeoutMs = 30000;
    bool verifyTls = true;
    std::string lastCollectionRoot; // thư mục collection mở gần nhất (mở lại khi khởi động)
    std::string fontName;           // font hiển thị (rỗng = mặc định); lấy từ Settings
    int fontSize = 11;

    // --- Response cache (USER layer — sửa trong Settings; kẹp ≤ ENV max). RESPONSE_CACHE.md §1 ---
    int ramCacheSizeMb = 64;        // mức RAM cache vận hành
    int diskCacheSizeMb = 256;      // mức disk cache vận hành
    bool cacheResponses = true;     // bật/tắt cache response
    bool cachePersist = true;       // giữ cache qua restart (tắt -> chỉ RAM, không gắn disk)
};

// ---- Session app-state ----
struct Session {
    int schemaVersion = 1;
    std::string lastOpenedFile; // relative
    std::string activeEnv = "Global";
};

// Handle theo dõi/huỷ request đang bay (README §3 threading).
using RequestHandle = std::uint64_t;

struct Progress {
    std::int64_t downloadTotal = 0;
    std::int64_t downloadNow = 0;
    std::int64_t uploadTotal = 0;
    std::int64_t uploadNow = 0;
};

} // namespace core
