// core/domain/response/interaction.hpp — transport-free request/stream classification consumed by the
// app + UI (RESTRUCTURE_PLAN S2). Lives in domain/response so the streaming vocabulary is unified here
// rather than in a separate top-level streaming/ tree. The transport-internal callback DTOs (StreamEvent,
// IStreamSink, …) stay private in infra/transport/shared.
#pragma once

namespace core {

// Terminal status of a stream/session (UI status pill + onStreamClose).
enum class StreamStatus { Ok, Error, Cancelled, Timeout };

// Request interaction classification (UI send routing). Transport-free.
enum class InteractionKind { Unary, ServerStream, ClientStream, BiDi, Duplex /* WebSocket session */ };

} // namespace core
