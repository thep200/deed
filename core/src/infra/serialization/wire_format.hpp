// Body "mode" tokens of the on-disk request JSON; request-type tokens live in request_type.hpp — the block key IS the type token.
#pragma once

namespace core::wire {

inline constexpr const char *kBodyNone = "none";
inline constexpr const char *kBodyJson = "json";
inline constexpr const char *kBodyText = "text";
inline constexpr const char *kBodyXml = "xml";
inline constexpr const char *kBodyForm = "form-urlencoded";
inline constexpr const char *kBodyMultipart = "multipart";
inline constexpr const char *kBodyBinary = "binary";

} // namespace core::wire
