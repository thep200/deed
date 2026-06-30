#include "infra/senders/ws_sender_adapter.hpp"

#include <cstdint>
#include <exception>
#include <variant>
#include <vector>

#include "core/streaming/i_stream_channel.hpp"
#include "core/streaming/i_stream_sink.hpp"

namespace core::infra {
namespace d = core::domain;

namespace {
// Legacy inbound IStreamSink -> domain ResponseEvents. Lives on the stack for one wsRun() call.
struct WsInboundTranslator final : core::IStreamSink {
  d::IResponseSink *sink;

  void onStreamOpen(const core::StreamMeta &m) override {
    std::vector<d::ResponseHeader> hs;
    for (const auto &kv : m.leading) hs.push_back({kv.key, kv.value});
    if (!hs.empty()) sink->emit(d::ResponseEvent(d::EvMetadata{std::move(hs)}));
  }
  void onStreamEvent(const core::StreamEvent &ev) override {
    auto kind = (ev.kind == core::StreamPayloadKind::Binary) ? d::WsSendKind::Binary : d::WsSendKind::Text;
    sink->emit(d::ResponseEvent(d::EvMessage{kind, ev.payload, ev.seq}));
  }
  void onStreamClose(const core::StreamEnd &end) override {
    if (end.status == core::StreamStatus::Error || end.status == core::StreamStatus::Timeout) {
      d::ErrorKind k = (end.status == core::StreamStatus::Timeout) ? d::ErrorKind::Timeout
                                                                   : d::ErrorKind::Protocol;
      sink->emit(d::ResponseEvent(d::EvFailed{{k, end.statusMessage, end.statusCode}}));
    } else {
      sink->emit(d::ResponseEvent(d::EvClosed{end.statusCode, end.statusMessage}));
    }
  }
};
} // namespace

d::Status WsSenderAdapter::execute(const d::RequestModel &resolved, d::IResponseSink &sink,
                                   const d::ICancellationToken &cancel) {
  // The model reaching here is already {{var}}-resolved (CoreApiClient::send -> DomainVariableResolver).
  if (resolved.type() != d::RequestType::WebSocket) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Unsupported, "not a websocket request", {}}}));
    return d::ok();
  }
  const d::WebSocketRequest &w = std::get<d::WebSocketRequest>(resolved.payload());

  // .env base + per-request RequestConfig (TLS verify + idle timeout) — mirrors Engine::buildWsConfig.
  core::WsConfig cfg = base_;
  cfg.verifyTls = resolved.config().tlsEnabledDefault;
  if (resolved.config().timeout.millis() > 0)
    cfg.idleTimeoutMs = static_cast<int>(resolved.config().timeout.millis());

  const std::string sid = "ws-" + std::to_string(nextId_++);

  auto session = wsMakeSession(cfg);
  auto channel = wsMakeChannel(session);
  {
    std::lock_guard<std::mutex> lk(mu_);
    channel_ = channel;
  }
  if (cancel.cancelled()) channel->close(1000, "cancelled"); // pre-flight cancel

  WsInboundTranslator translator;
  translator.sink = &sink;
  try {
    // Runs the recv/send pump on THIS thread; emits onStreamOpen/Event/Close on the translator, then returns
    // after the close handshake (keeps the saga's pool worker parked for the whole session).
    wsRun(w, translator, session, sid);
  } catch (const std::exception &e) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Internal, e.what(), {}}}));
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    channel_.reset();
  }
  return d::ok();
}

d::Status WsSenderAdapter::push(d::WsMessage m) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!channel_) return d::Status::fail({d::ErrorCode::Unsupported, "no open ws session"});
  bool ok = (m.kind == d::WsSendKind::Binary)
                ? channel_->sendBinary(std::vector<std::uint8_t>(m.payload.begin(), m.payload.end()))
                : channel_->sendText(m.payload);
  return ok ? d::ok() : d::Status::fail({d::ErrorCode::Internal, "ws send queue full"});
}

d::Status WsSenderAdapter::close(int code, std::string reason) {
  std::lock_guard<std::mutex> lk(mu_);
  if (channel_) channel_->close(code, reason);
  return d::ok();
}

} // namespace core::infra
