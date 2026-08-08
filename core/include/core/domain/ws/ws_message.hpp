#pragma once

#include <string>

namespace core::domain {

enum class WsSendKind { Text, Binary };

// For Binary the payload encoding (base64/hex) is a mapper convention; the domain carries bytes-as-string.
struct WsMessage {
  WsSendKind kind = WsSendKind::Text;
  std::string payload;
  bool operator==(const WsMessage &o) const { return kind == o.kind && payload == o.payload; }
  bool operator!=(const WsMessage &o) const { return !(*this == o); }
};

} // namespace core::domain
