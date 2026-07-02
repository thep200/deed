// core/domain/response/response_event.hpp — the event stream for unary AND streaming sends
// (REFACTOR_SPEC §5.9). One variant carried by IRequestObserver/IResponseSink across the whole lifecycle.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/domain/kafka/kafka_consume.hpp"
#include "core/domain/response/api_error.hpp"
#include "core/domain/response/api_response.hpp"
#include "core/domain/ws/ws_message.hpp"

namespace core::domain {

struct EvStarted {
  std::chrono::milliseconds at{0};
  bool operator==(const EvStarted &o) const { return at == o.at; }
};
struct EvMetadata {
  std::vector<ResponseHeader> headers; // initial metadata / response headers
  bool operator==(const EvMetadata &o) const { return headers == o.headers; }
};
struct EvMessage {
  WsSendKind kind = WsSendKind::Text;
  std::string payload; // one message / chunk / SSE event
  std::size_t index = 0;
  bool operator==(const EvMessage &o) const {
    return kind == o.kind && payload == o.payload && index == o.index;
  }
};
struct EvProgress {
  std::uint64_t bytes = 0;
  std::optional<double> fraction;
  bool operator==(const EvProgress &o) const { return bytes == o.bytes && fraction == o.fraction; }
};
struct EvTrailers {
  std::vector<ResponseHeader> trailers; // gRPC trailing metadata
  bool operator==(const EvTrailers &o) const { return trailers == o.trailers; }
};
struct EvCompleted {
  ApiResponse summary;
  bool operator==(const EvCompleted &o) const { return summary == o.summary; }
};
struct EvFailed {
  ApiError error;
  bool operator==(const EvFailed &o) const { return error == o.error; }
};
struct EvClosed {
  std::optional<int> code;
  std::string reason; // WS close
  bool operator==(const EvClosed &o) const { return code == o.code && reason == o.reason; }
};
// One Kafka consumer record — OUTPUT THUẦN (SPEC_kafka §3/§6): the record travels byte-for-byte, no
// EvMessage string-encoding detour (KafkaRecord has too much shape — topic/partition/offset/headers/ts —
// to flatten into EvMessage's {kind,payload,index} without processing the bytes).
struct EvKafkaRecord {
  KafkaRecord record;
  bool operator==(const EvKafkaRecord &o) const { return record == o.record; }
};

class ResponseEvent {
public:
  using Variant = std::variant<EvStarted, EvMetadata, EvMessage, EvProgress, EvTrailers, EvCompleted,
                               EvFailed, EvClosed, EvKafkaRecord>;

  ResponseEvent(Variant v) : data_(std::move(v)) {} // implicit: emit(EvCompleted{...}) reads naturally

  template <class V> decltype(auto) match(V &&v) const { return std::visit(std::forward<V>(v), data_); }

  // Lifecycle helpers used by the saga/orchestrator.
  bool isTerminal() const {
    return std::holds_alternative<EvCompleted>(data_) || std::holds_alternative<EvFailed>(data_) ||
           std::holds_alternative<EvClosed>(data_);
  }
  template <class T> bool is() const { return std::holds_alternative<T>(data_); }
  template <class T> const T *get() const { return std::get_if<T>(&data_); }

  bool operator==(const ResponseEvent &o) const { return data_ == o.data_; }
  bool operator!=(const ResponseEvent &o) const { return !(data_ == o.data_); }

private:
  Variant data_;
};

} // namespace core::domain
