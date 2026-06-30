#include "infra/transport/graphql/native_graphql_sender.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "infra/transport/graphql/gql_ws_protocol.hpp" // GraphQlWsProtocol (graphql-transport-ws / graphql-ws)
#include "infra/transport/shared/cancel_token.hpp"
#include "infra/transport/shared/i_stream_channel.hpp"
#include "infra/transport/shared/i_stream_sink.hpp"
#include "infra/transport/graphql/graphql.hpp"               // gql::effectiveOperation / gql::buildHttpModel (domain)
#include "infra/transport/ws/ws_sender.hpp"             // wsMakeSession/wsMakeChannel/wsRunProtocol (shared WS pump)

namespace core::infra {
namespace d = core::domain;

namespace {

// Legacy IStreamSink -> domain ResponseEvent (the translation the retired LegacySenderAdapter used).
struct GqlStreamSink final : core::IStreamSink {
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
    if (!end.trailing.empty()) {
      std::vector<d::ResponseHeader> ts;
      for (const auto &kv : end.trailing) ts.push_back({kv.key, kv.value});
      sink->emit(d::ResponseEvent(d::EvTrailers{std::move(ts)}));
    }
    if (end.status == core::StreamStatus::Ok) {
      d::ApiResponse r;
      r.statusCode = end.statusCode;
      r.elapsed = std::chrono::milliseconds(end.elapsedMs);
      sink->emit(d::ResponseEvent(d::EvCompleted{std::move(r)}));
    } else {
      d::ErrorKind k = (end.status == core::StreamStatus::Cancelled) ? d::ErrorKind::Cancelled
                       : (end.status == core::StreamStatus::Timeout) ? d::ErrorKind::Timeout
                                                                     : d::ErrorKind::Protocol;
      sink->emit(d::ResponseEvent(d::EvFailed{{k, end.statusMessage, end.statusCode}}));
    }
  }
};

} // namespace

d::Status NativeGraphQlSender::execute(const d::RequestModel &resolved, d::IResponseSink &sink,
                                       const d::ICancellationToken &cancel) {
  const d::GraphQlRequest &dg = std::get<d::GraphQlRequest>(resolved.payload());
  // Subscription over WebSocket -> native graphql-transport-ws path; everything else is HTTP.
  const bool isSubscription = gql::effectiveOperation(dg) == d::GqlOperationType::Subscription &&
                              dg.subTransport() == d::GqlSubTransport::Ws;
  if (isSubscription) return runSubscription(resolved, sink, cancel);

  // query/mutation: repackage as an HTTP request (proven helper) and run it natively (cpr on domain types).
  return http_.execute(gql::buildHttpModel(resolved), sink, cancel);
}

// Subscription over WebSocket: open a WS with the graphql-ws subprotocol, drive GraphQlWsProtocol on the raw
// frames (it owns the §3 open/close contract on its sink), and translate to domain events. Ported from the
// retired core::GraphQlSender::openStream — same ws_sender pump + protocol, now Engine/adapter-free.
d::Status NativeGraphQlSender::runSubscription(const d::RequestModel &resolved, d::IResponseSink &sink,
                                               const d::ICancellationToken &cancel) {
  const d::GraphQlRequest &dg = std::get<d::GraphQlRequest>(resolved.payload());

  core::WsConfig cfg;
  cfg.verifyTls = resolved.config().tlsEnabledDefault; // wss:// cert verification from the per-request Config
  auto session = core::wsMakeSession(cfg);
  auto channel = core::wsMakeChannel(session);

  auto token = std::make_shared<core::CancelToken>();
  if (cancel.cancelled()) token->cancel();
  {
    std::lock_guard<std::mutex> lk(mu_);
    wsSession_ = session;
    wsToken_ = token;
  }

  GqlStreamSink gsink;
  gsink.sink = &sink;
  core::GraphQlWsProtocol proto("gqlsub", dg, &gsink,
                                [channel](const std::string &s) { channel->sendText(s); });

  core::WsFrameHooks hooks;
  hooks.onOpen = [&proto] { proto.onOpen(); }; // -> connection_init
  hooks.onText = [&proto, session](const std::string &t) {
    proto.onFrame(t);
    if (proto.closed()) core::wsRequestClose(session, 1000, ""); // server completed -> close the socket
  };
  hooks.onClose = [&proto](core::StreamStatus st, int code, const std::string &m) {
    proto.onClose(st, code, m); // idempotent -> at most one close on the sink
  };

  // Synthesized domain WebSocket request (url + headers + auth + the graphql-ws subprotocol) for the shared
  // WS pump — built from the DOMAIN graphql payload (url/headers/auth carried as domain VOs already).
  auto wurl = d::Url::createWithSchemes(dg.url().raw(), {"ws", "wss"});
  if (!wurl) {
    sink.emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Parse, wurl.error().message, {}}}));
    std::lock_guard<std::mutex> lk(mu_);
    wsSession_.reset();
    wsToken_.reset();
    return d::ok();
  }
  // Parts holds a Url (no default ctor) -> brace-init in member order: url, subprotocols, headers, auth,
  // onOpenSend, defaultSendKind.
  d::WebSocketRequest::Parts wp{
      wurl.take(),
      {proto.legacy() ? std::string("graphql-ws") : std::string("graphql-transport-ws")},
      dg.headers(),
      dg.auth(),
      {},
      d::WsSendKind::Text};
  auto wreq = d::WebSocketRequest::create(std::move(wp));

  core::wsRunProtocol(wreq.value(), hooks, session, "gqlsub", token); // blocks for the subscription lifetime
  {
    std::lock_guard<std::mutex> lk(mu_);
    wsSession_.reset();
    wsToken_.reset();
  }
  return d::ok();
}

d::Status NativeGraphQlSender::close(int code, std::string reason) {
  http_.close(code, reason); // cancel an in-flight HTTP query/mutation
  std::lock_guard<std::mutex> lk(mu_);
  if (wsToken_) wsToken_->cancel();
  if (wsSession_) core::wsRequestClose(wsSession_, code ? code : 1000, reason);
  return d::ok();
}

} // namespace core::infra
