// core/src/infra/senders/ws_sender_adapter.hpp — REFACTOR_SPEC native-rewrite (WebSocket transport).
// Runs the libcurl WebSocket pump (wsMakeSession/wsMakeChannel/wsRun from ws_sender.cpp) DIRECTLY on the
// domain model — no longer via Engine::openSession. execute() runs wsRun on the calling (saga pool) thread,
// which BLOCKS for the whole session (keeping the saga alive so push/close from other threads reach the
// channel); inbound frames become EvMessage, the close becomes EvClosed/EvFailed. Auth is applied by
// ws_sender itself (applyAuthHeaders on the handshake), so the already-resolved domain model is enough.
// The .env WS tunables arrive as a pre-built WsConfig base (composition root maps them from Engine::wsLimits);
// per-request TLS/timeout from RequestConfig are layered on at execute time. One session per adapter.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/domain/ports/driven/i_request_sender.hpp"
#include "sending/ws_sender.hpp" // WsConfig (base from .env) — internal core/src header

namespace core {
class IStreamChannel;
} // namespace core

namespace core::infra {

class WsSenderAdapter final : public domain::IRequestSender {
public:
  explicit WsSenderAdapter(core::WsConfig base) : base_(base) {}

  bool supports(domain::RequestType t) const override { return t == domain::RequestType::WebSocket; }
  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;
  domain::Status push(domain::WsMessage) override;
  domain::Status close(int code, std::string reason) override;

private:
  core::WsConfig base_;                              // .env tunables; per-request TLS/timeout layered at execute
  std::uint64_t nextId_ = 1;                         // session-id counter (one session at a time)
  std::mutex mu_;                                    // guards channel_ (set by execute, read by push/close)
  std::shared_ptr<core::IStreamChannel> channel_;   // send side of the active session
};

} // namespace core::infra
