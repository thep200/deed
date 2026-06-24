// core/request_model.hpp — the request side of the neutral model (README §7): protocol classification,
// per-transport request blocks, per-request config, and the RequestModel envelope. Do NOT leak
// grpc++/protobuf/libcurl/nlohmann here — this is the port the UI sees.
#pragma once

#include <string>
#include <vector>

#include "core/dto_common.hpp"

namespace core {

// Protocol classification (matches the "type" field in the request file).
enum class RequestType { Http, Grpc, WebSocket, GraphQL };

std::string toString(RequestType);
bool parseRequestType(const std::string &, RequestType &out);

// ---- HTTP Body (tagged union by mode) ----
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
  std::string type = "none"; // none | basic | bearer | apikey
  std::string basicUsername;
  std::string basicPassword;
  std::string bearerToken;
  std::string apikeyKey;
  std::string apikeyValue;
  std::string apikeyIn = "header"; // header | query
};

// Apply an Auth to a header list (bearer -> Authorization: Bearer; basic -> Authorization: Basic base64;
// apikey "in header" -> the named header). Used by transports without per-message headers (WS / GraphQL
// subscription handshake). apikey "in query" is left to the URL layer.
void applyAuthHeaders(const Auth &auth, std::vector<KeyValue> &headers);

struct HttpSettings {
  int timeoutMs = 30000;
  bool followRedirects = true;
  bool verifyTls = true;
  bool timeoutMsSet = false; // lets merge precedence know if the field was set
  bool followRedirectsSet = false;
  bool verifyTlsSet = false;
};

// Server-Sent Events mode (SPEC_sse §1). SSE is a *consumption mode* of an HTTP request, not a new type.
// None: plain unary (gather body). Sse: always parse the response as text/event-stream. Auto: stream-parse
// only if the response Content-Type is text/event-stream, else unary. Default None so ordinary HTTP
// requests stay unary (Auto would route every request through the stream path).
enum class HttpStreamMode { None, Auto, Sse };

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<KeyValue> pathVariables;
  std::vector<KeyValue> params;
  std::vector<KeyValue> headers;
  Body body;
  Auth auth;
  HttpSettings settings;
  HttpStreamMode streamMode = HttpStreamMode::None; // SSE consumption mode (SPEC_sse §1)
};

// SSE detection (SPEC_sse §1/§4). A request streams if streamMode is Sse/Auto, OR it carries an enabled
// `Accept: text/event-stream` header (the standard SSE trigger) — so users can opt in with just a header.
bool httpRequestsSse(const HttpRequest &h); // route to the stream path (openStream)
bool httpForcesSse(const HttpRequest &h);   // parse as SSE regardless of response Content-Type

// ---- gRPC ----
struct ProtoSource {
  // reflection | protoFiles | descriptorSet
  std::string mode = "reflection";
  std::vector<std::string> files;       // protoFiles
  std::vector<std::string> importPaths; // protoFiles
  std::string descriptorSetPath;        // descriptorSet
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
  std::string service;    // full pkg.Service
  std::string method;     // RPC name
  std::string methodType; // unary | server_streaming | client_streaming | bidi_streaming
};

struct GrpcRequest {
  std::string target;  // host:port (authority, not a URL with path)
  std::string service; // full pkg.Service
  std::string method;  // RPC name
  std::string methodType = "unary"; // unary | server_streaming | client_streaming | bidi_streaming
  ProtoSource protoSource;
  std::string message; // JSON; Core marshals to protobuf on send
  std::vector<KeyValue> metadata;
  GrpcTls tls;
  GrpcSettings settings;
};

// ---- WebSocket (SPEC_websocket §1) ----
enum class WsSendKind { Text, Binary };

struct WsRequest {
  std::string url;                       // ws:// or wss://
  std::vector<KeyValue> headers;         // handshake headers (Authorization, …)
  std::vector<std::string> subprotocols; // Sec-WebSocket-Protocol
  std::vector<std::string> onOpenSend;   // optional: messages to send right after open (e.g. subscribe)
  WsSendKind defaultSendKind = WsSendKind::Text;
  Auth auth; // applied to the handshake (Authorization header / apikey)
};

// ---- GraphQL (SPEC_graphql) — an application layer over HTTP (query/mutation) or WS/SSE (subscription) ----
enum class GqlOperation { Query, Mutation, Subscription, Auto }; // Auto: inferred from the query text
enum class GqlSubTransport { WebSocket, Sse };                   // transport for subscription
enum class GqlWsProtocol { GraphQlTransportWs, SubscriptionsTransportWs /* legacy */ };

struct GraphQlRequest {
  std::string url;   // endpoint (https://host/graphql ; wss://host/graphql for sub)
  std::string query; // the GraphQL document
  std::string variablesJson = "{}";
  std::string operationName; // optional (when a document holds multiple operations)
  GqlOperation operation = GqlOperation::Auto;

  // subscription only:
  GqlSubTransport subTransport = GqlSubTransport::WebSocket;
  GqlWsProtocol wsProtocol = GqlWsProtocol::GraphQlTransportWs;
  std::string connectionInitPayloadJson; // auth payload for connection_init (§6)

  std::vector<KeyValue> headers; // reused HTTP headers (Authorization, …)
  bool useGetForQuery = false;   // optional GET for query
  Auth auth; // applied to query/mutation (HTTP) and subscription handshake
};

// ---- Per-request config (envelope level; edited in the "Config" tab) ----
// One unified place for timeout + TLS, applied to whichever transport the request uses.
//   timeoutMs -> HTTP/GraphQL request timeout | gRPC deadline | WebSocket idle timeout
//   tls       -> HTTP/WS/GraphQL: verify the server TLS certificate
//                gRPC: use a TLS channel (replaces the old per-connection TLS toggle)
struct RequestConfig {
  int timeoutMs = 1800000; // 30 min
  bool tls = true;
};

// ---- Model of one request (envelope + per-type block) ----
struct RequestModel {
  int schemaVersion = 1;
  std::string id;
  std::string name;
  std::string description;
  RequestType type = RequestType::Http;
  int seq = 0;
  RequestConfig config; // per-request timeout + TLS (Config tab)

  HttpRequest http;       // used when type == Http
  GrpcRequest grpc;       // used when type == Grpc
  WsRequest ws;           // used when type == WebSocket
  GraphQlRequest graphql; // used when type == GraphQL
};

} // namespace core
