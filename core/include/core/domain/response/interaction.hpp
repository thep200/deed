#pragma once

namespace core {

// Terminal status of a stream/session.
enum class StreamStatus { Ok, Error, Cancelled, Timeout };

enum class InteractionKind { Unary, ServerStream, ClientStream, BiDi, Duplex /* WebSocket session */ };

} // namespace core
