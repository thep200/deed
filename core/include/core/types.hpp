// core/types.hpp — Core's neutral DTOs (README §7).
// UI only sees these structs; do NOT leak grpc++/protobuf/libcurl/nlohmann types out the port.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace core {

// ---- Protocol classification (matches the "type" field in the request file) ----
enum class RequestType { Http, Grpc };

std::string toString(RequestType);
bool parseRequestType(const std::string&, RequestType& out);

// ---- key/value line (headers, params, metadata, pathVariables, form...) ----
// Array (not object) to keep order, allow duplicate keys, each line has enabled. (README §7.4)
struct KeyValue {
    std::string key;
    std::string value;
    bool enabled = true;
};

// ---- HTTP Body (tagged union by mode) ----
struct MultipartPart {
    std::string key;
    std::string value;      // used when type == "text"
    std::string type;       // "text" | "file"
    std::string filePath;   // used when type == "file"
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

// ---- HTTP Auth ----
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
    bool timeoutMsSet = false;          // lets merge precedence know if the field was set
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

// One available RPC (for UI to build the service/method dropdown).
struct GrpcMethodInfo {
    std::string service;     // full pkg.Service
    std::string method;      // RPC name
    std::string methodType;  // unary | server_streaming | client_streaming | bidi_streaming
};

struct GrpcRequest {
    std::string target;          // host:port (authority, not a URL with path)
    std::string service;         // full pkg.Service
    std::string method;          // RPC name
    std::string methodType = "unary"; // unary | server_streaming | client_streaming | bidi_streaming
    ProtoSource protoSource;
    std::string message;         // JSON; Core marshals to protobuf on send
    std::vector<KeyValue> metadata;
    GrpcTls tls;
    GrpcSettings settings;
};

// ---- Model of one request (envelope + per-type block) ----
struct RequestModel {
    int schemaVersion = 1;
    std::string id;
    std::string name;
    std::string description;
    RequestType type = RequestType::Http;
    int seq = 0;

    HttpRequest http;   // used when type == Http
    GrpcRequest grpc;   // used when type == Grpc
};

// ResolvedRequest: RequestModel after resolving {{var}} + applying auth. (README §8.1)
// Shares the same struct for brevity — sender only receives the resolved one.
struct ResolvedRequest {
    RequestModel model;
};

// ---- Call result ----
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
    std::vector<Cookie> cookies;     // Set-Cookie of the current response (POC, no jar)
    // common
    std::string body;                // HTTP body or gRPC message JSON
    long elapsedMs = 0;
    std::int64_t sizeBytes = 0;
    std::string resolvedRequestDump; // resolved request (Request tab for debugging)
};

enum class ErrorKind { Network, Timeout, Tls, Cancelled, Parse, Unsupported, Unknown };
std::string toString(ErrorKind);

struct ApiError {
    ErrorKind kind = ErrorKind::Unknown;
    std::string message;
};

// ---- Lightweight JSON validate (UI spec §7) ----
struct ValidationResult {
    bool ok = true;
    int line = 0;
    int col = 0;
    std::string msg;
};

// ---- Collection tree (lazy, metadata only) ----
struct TreeNode {
    std::string name;       // display name (request: name field; folder: directory name)
    std::string relPath;    // path relative to collection root
    bool isFolder = false;
    // present only when !isFolder:
    std::string id;         // stable request id (survives rename/move)
    RequestType requestType = RequestType::Http;
    std::string methodOrType; // HTTP method, or gRPC methodType, to show as a badge
    std::vector<TreeNode> children;
};

// ---- Environment ----
struct EnvKey {
    std::string key;
    std::string value;
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
    std::string lastCollectionRoot; // most recently opened collection dir (reopened at startup)
    std::string fontName;           // display font (empty = default); from Settings
    int fontSize = 11;

    // --- Response cache (USER layer — edited in Settings; clamped ≤ ENV max). RESPONSE_CACHE.md §1 ---
    int ramCacheSizeMb = 64;        // operating RAM cache level
    int diskCacheSizeMb = 256;      // operating disk cache level
    bool cacheResponses = true;     // enable/disable response cache
    bool cachePersist = true;       // keep cache across restart (off -> RAM only, no disk attached)
};

// ---- Session app-state ----
struct Session {
    int schemaVersion = 1;
    std::string lastOpenedFile; // relative
    std::string activeEnv = "Global";
};

// Handle to track/cancel an in-flight request (README §3 threading).
using RequestHandle = std::uint64_t;

struct Progress {
    std::int64_t downloadTotal = 0;
    std::int64_t downloadNow = 0;
    std::int64_t uploadTotal = 0;
    std::int64_t uploadNow = 0;
};

} // namespace core
