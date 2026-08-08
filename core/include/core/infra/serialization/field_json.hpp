// nlohmann lives ONLY in the .cpp — this public header stays dependency-clean so the UI can include it.
#pragma once

#include <string>

#include <vector>

#include "core/domain/auth/auth.hpp"
#include "core/domain/body/body.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/grpc/grpc_metadata.hpp"
#include "core/domain/kafka/kafka_consume.hpp"
#include "core/domain/kafka/kafka_produce.hpp"
#include "core/domain/ldap/ldap_request.hpp"
#include "core/domain/request/request_config.hpp"
#include "core/domain/response/api_response.hpp"
#include "core/domain/soap/soap_request.hpp"
#include "core/domain/values/header.hpp"
#include "core/domain/values/query_param.hpp"

namespace core::serial {

// Headers — JSON array [{key,value,enabled}] <-> HeaderList.
std::string headersToJson(const domain::HeaderList &);
domain::Result<domain::HeaderList> jsonToHeaders(const std::string &);

// Query params — JSON array [{key,value,enabled}] <-> QueryParamList.
std::string paramsToJson(const domain::QueryParamList &);
domain::Result<domain::QueryParamList> jsonToParams(const std::string &);

// Auth — flat JSON discriminated by "type" (none/basic/bearer/oauth2). Legacy nested sub-objects and "apikey" still read.
std::string authToJson(const domain::Auth &);
domain::Result<domain::Auth> jsonToAuth(const std::string &);

// Body — JSON {mode, json|text|xml | formUrlEncoded[] | multipart[] | binary{filePath}} <-> Body.
std::string bodyToJson(const domain::Body &);
domain::Result<domain::Body> jsonToBody(const std::string &);

// The UI body tab shows ONE mode's UNWRAPPED content — the {mode} lives in the app, not the text.
struct EditorBody {
  std::string mode;    // "json" | "text" | "xml" | "form-urlencoded" | "binary"
  std::string content; // unwrapped text for that mode
};
EditorBody bodyToEditor(const domain::Body &);
domain::Result<domain::Body> bodyFromEditor(const std::string &mode, const std::string &content);

// Config — JSON {timeout_ms, tls} <-> RequestConfig.
std::string configToJson(const domain::RequestConfig &);
domain::Result<domain::RequestConfig> jsonToConfig(const std::string &);

// Kafka only — JSON {timeout_ms}, NO "tls" key (Kafka has no TLS toggle); tlsEnabledDefault is pinned to false on parse.
std::string kafkaRequestConfigToJson(const domain::RequestConfig &);
domain::Result<domain::RequestConfig> jsonToKafkaRequestConfig(const std::string &);

// gRPC metadata — JSON array [{key,value,enabled}] <-> GrpcMetadata.
std::string metadataToJson(const domain::GrpcMetadata &);
domain::Result<domain::GrpcMetadata> jsonToMetadata(const std::string &);

// Response headers — domain ResponseHeader list -> JSON array [{key,value}] (display only; read-only).
std::string responseHeadersToJson(const std::vector<domain::ResponseHeader> &);

// Display only — `value` is embedded as parsed JSON when it parses, else as a raw string; record bytes untouched.
std::string kafkaRecordToDisplayJson(const domain::KafkaRecord &);

// The on-disk mapper embeds these same shapes under kafka.producer.{config,message} / consumer.config.
std::string kafkaMessageToJson(const domain::KafkaMessage &);
domain::Result<domain::KafkaMessage> jsonToKafkaMessage(const std::string &);
std::string kafkaProduceConfigToJson(const domain::KafkaProduceConfig &);
domain::Result<domain::KafkaProduceConfig> jsonToKafkaProduceConfig(const std::string &);
std::string kafkaConsumeConfigToJson(const domain::KafkaConsumeConfig &);
domain::Result<domain::KafkaConsumeConfig> jsonToKafkaConsumeConfig(const std::string &);

// Everything except the URL (the URL bar owns that) — flat JSON, keys mirroring the struct below.
struct LdapParams {
  bool startTls = false;
  std::string bindDn, bindPassword, baseDn;
  domain::LdapScope scope = domain::LdapScope::Sub;
  std::string filter;
  std::vector<std::string> attributes;
  std::string group, testPassword;
  int sizeLimit = 100, timeLimit = 10, pageSize = 500;
};
std::string ldapParamsToJson(const domain::LdapRequest &);
domain::Result<LdapParams> jsonToLdapParams(const std::string &);

// SOAP editor tab: {action, version:"1.1"|"1.2"}. Editor-facing, not the request file.
struct SoapConfig {
  std::string action;
  domain::SoapVersion version = domain::SoapVersion::V1_1;
};
std::string soapConfigToJson(const domain::SoapRequest &);
domain::Result<SoapConfig> jsonToSoapConfig(const std::string &);

std::string formatJson(const std::string &text, bool pretty); // unparseable -> XML indent if XML-ish (pretty), else verbatim
std::string formatXml(const std::string &text); // conservative indenter; anything unusual -> verbatim
std::string jsonEncodeString(const std::string &text);        // text -> "\"...\""
std::string jsonDecodeString(const std::string &text);        // "\"...\"" -> inner; else verbatim

} // namespace core::serial
