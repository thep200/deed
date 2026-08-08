// Transport-internal DTOs a sender emits to IStreamSink; the app/UI only ever sees the domain ResponseEvent.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/response/interaction.hpp"
#include "infra/transport/shared/dto_common.hpp"

namespace core {

enum class StreamTransport { Grpc, Sse, WebSocket };
enum class StreamPayloadKind { Json, Text, Binary }; // Binary -> payload is base64

// Defaults keep gRPC server-streaming unchanged: direction Inbound, frameType Message.
enum class StreamDirection { Inbound, Outbound };
enum class StreamFrameType { Text, Binary, Ping, Pong, Close, Message /*gRPC*/ };

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
  // display/telemetry only — the UI must not branch on it
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
  bool truncated = false; // true if a configured ceiling was hit
};

} // namespace core
