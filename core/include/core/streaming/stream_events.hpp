// core/streaming/stream_events.hpp — the transport-internal streaming-callback DTOs (SPEC_grpc_streaming §3).
// These are the model a SENDER emits to an IStreamSink; the app/UI never sees them (it consumes the domain
// ResponseEvent — the native senders' translators convert StreamEvent/Meta/End -> domain). Kept out of the
// legacy types.hpp cluster so the WS pump + gql-ws protocol survive the types.hpp removal (REFACTOR_SPEC P6).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/dto_common.hpp" // KeyValue (neutral key/value pair)

namespace core {

enum class StreamTransport { Grpc, Sse, WebSocket };
enum class StreamPayloadKind { Json, Text, Binary }; // Binary -> payload is base64
enum class StreamStatus { Ok, Error, Cancelled, Timeout };

// SPEC_websocket §2.1 — duplex deltas. Defaults keep gRPC server-streaming unchanged: direction defaults
// Inbound, frameType defaults Message.
enum class StreamDirection { Inbound, Outbound };
enum class StreamFrameType { Text, Binary, Ping, Pong, Close, Message /*gRPC*/ };

// One neutral event — the shared unit of data for every transport.
struct StreamEvent {
  std::uint64_t seq = 0; // 0-based, monotonic within one stream (UI appends in order)
  StreamDirection direction = StreamDirection::Inbound; // WS: Inbound | Outbound (others: Inbound)
  StreamFrameType frameType = StreamFrameType::Message; // WS frame kind; gRPC: Message
  StreamPayloadKind kind = StreamPayloadKind::Json;
  std::string payload; // text JSON expected for the response pane
  std::string name;    // optional: gRPC "message" | SSE event name
  std::string id;      // optional: SSE id (resume / Last-Event-ID later)
  std::vector<KeyValue> metadata; // optional: per-event metadata
  long long offsetMs = 0;         // ms since the stream opened
};

struct StreamMeta { // emitted at onStreamOpen
  std::string streamId;
  // display/telemetry ONLY — UI must NOT branch on it (INV-1)
  StreamTransport transport = StreamTransport::Grpc;
  std::vector<KeyValue> leading; // gRPC leading metadata | SSE/HTTP headers
  long long startedAtEpochMs = 0;
};

struct StreamEnd { // emitted at onStreamClose
  StreamStatus status = StreamStatus::Ok;
  int statusCode = 0; // gRPC status code | HTTP status
  std::string statusMessage;
  std::vector<KeyValue> trailing; // gRPC trailing metadata
  std::uint64_t totalEvents = 0;
  std::uint64_t totalBytes = 0;
  long long elapsedMs = 0;
  bool truncated = false; // true if a configured ceiling was hit (§9)
};

// Request interaction classification (UI send routing). Transport-free; relocated here so it survives the
// legacy types.hpp removal (the rest of streaming_dto.hpp — ResolvedRequest — dies with RequestModel).
enum class InteractionKind { Unary, ServerStream, ClientStream, BiDi, Duplex /* WebSocket session */ };

} // namespace core
