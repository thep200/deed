// wire_format.hpp — INTERNAL (core/src). Single source of truth for the on-disk request JSON
// discriminant tokens (request "type" + per-type block key, body "mode"). Centralized so the mapper /
// field codec don't repeat magic string literals (SPEC_refactor §3.3 — no scattered constants).
#pragma once

namespace core::wire {

// Request type token: the JSON "type" field value AND the per-type payload block key.
inline constexpr const char *kHttp = "http";
inline constexpr const char *kGrpc = "grpc";
inline constexpr const char *kWs = "ws";
inline constexpr const char *kGraphql = "graphql";
inline constexpr const char *kKafka = "kafka";

// Body "mode" token (HTTP body discriminant).
inline constexpr const char *kBodyNone = "none";
inline constexpr const char *kBodyJson = "json";
inline constexpr const char *kBodyText = "text";
inline constexpr const char *kBodyXml = "xml";
inline constexpr const char *kBodyForm = "form-urlencoded";
inline constexpr const char *kBodyMultipart = "multipart";
inline constexpr const char *kBodyFormData = "form-data"; // legacy alias accepted on read
inline constexpr const char *kBodyBinary = "binary";

} // namespace core::wire
