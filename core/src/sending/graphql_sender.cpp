#include "sending/graphql_sender.hpp"

#include "core/graphql/gql_ws_protocol.hpp"
#include "graphql/graphql.hpp"
#include "sending/ws_sender.hpp"

namespace core {

// query/mutation: the Engine normally pre-converts to HTTP before sender lookup; convert here too so a
// direct call is still correct.
void GraphQlSender::send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
                         const std::shared_ptr<CancelToken>& cancel) {
    ResolvedRequest hr = req;
    hr.model = gql::buildHttpModel(req.model);
    http_.send(hr, handle, delegate, cancel);
}

bool GraphQlSender::isStreaming(const ResolvedRequest& req) const {
    return gql::effectiveOperation(req.model.graphql) == GqlOperation::Subscription;
}

// Subscription over WebSocket: open a WS with the graphql-ws subprotocol, then drive GraphQlWsProtocol on
// top of the raw frames. The protocol owns the §3 open/close contract on `sink`.
void GraphQlSender::openStream(const ResolvedRequest& req, IStreamSink& sink,
                               const std::shared_ptr<CancelToken>& cancel) {
    const GraphQlRequest& g = req.model.graphql;
    if (g.subTransport != GqlSubTransport::WebSocket) {
        sink.onStreamOpen(StreamMeta{req.streamId, StreamTransport::Sse, {}, 0});
        StreamEnd end;
        end.status = StreamStatus::Error;
        end.statusMessage = "GraphQL subscription over SSE is not supported yet (use WebSocket).";
        sink.onStreamClose(end);
        return;
    }

    WsConfig cfg;
    cfg.verifyTls = req.model.config.tls;   // wss:// cert verification from the per-request Config
    auto session = wsMakeSession(cfg);
    auto channel = wsMakeChannel(session);

    // sendRaw enqueues a WS text frame; the protocol's sink is the real UI sink.
    GraphQlWsProtocol proto(req.streamId, g, &sink,
                            [channel](const std::string& s) { channel->sendText(s); });

    WsFrameHooks hooks;
    hooks.onOpen = [&proto] { proto.onOpen(); };   // -> connection_init
    hooks.onText = [&proto, session](const std::string& t) {
        proto.onFrame(t);
        if (proto.closed()) wsRequestClose(session, 1000, "");   // server completed -> close the socket
    };
    hooks.onClose = [&proto](StreamStatus st, int code, const std::string& msg) {
        proto.onClose(st, code, msg);   // idempotent (guarded) -> at most one close on the sink
    };

    // Reuse the WS transport via a synthesized WebSocket request (url + auth + the graphql-ws subprotocol).
    ResolvedRequest wsReq;
    wsReq.streamId = req.streamId;
    wsReq.model.type = RequestType::WebSocket;
    WsRequest& w = wsReq.model.ws;
    w.url = g.url;
    w.headers = g.headers;   // reuse GraphQL headers as handshake headers
    w.auth = g.auth;         // auth -> handshake header (bearer/basic/apikey)
    w.subprotocols = {proto.legacy() ? std::string("graphql-ws") : std::string("graphql-transport-ws")};

    wsRunProtocol(wsReq, hooks, session, req.streamId, cancel);
}

} // namespace core
