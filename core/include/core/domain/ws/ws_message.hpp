// core/domain/ws/ws_message.hpp — WsMessage value (REFACTOR_SPEC §5.7).
// For Binary the payload encoding (base64/hex) is a mapper convention; the domain just carries the bytes-as-string.
#pragma once

#include <string>

namespace core::domain {

enum class WsSendKind { Text, Binary };

struct WsMessage {
  WsSendKind kind = WsSendKind::Text;
  std::string payload;
  bool operator==(const WsMessage &o) const { return kind == o.kind && payload == o.payload; }
  bool operator!=(const WsMessage &o) const { return !(*this == o); }
};

} // namespace core::domain
