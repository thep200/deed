// gql_ws_protocol.cpp — graphql-transport-ws / legacy graphql-ws protocol state machine (SPEC_graphql §6).
#include "infra/transport/graphql/gql_ws_protocol.hpp"

#include <utility>

#include <nlohmann/json.hpp>

namespace core {

using nlohmann::json;

GraphQlWsProtocol::GraphQlWsProtocol(std::string streamId, core::domain::GraphQlRequest req,
                                     IStreamSink* uiSink, std::function<void(const std::string&)> sendRaw)
    : streamId_(std::move(streamId)), req_(std::move(req)), sink_(uiSink), sendRaw_(std::move(sendRaw)) {}

bool GraphQlWsProtocol::legacy() const {
    // subprotocol "graphql-ws" == the legacy subscriptions-transport-ws library (SPEC_graphql §6.1 naming).
    return req_.wsProtocol() == "graphql-ws";
}
// Legacy (subscriptions-transport-ws) uses connection_init/start/stop + data; modern uses
// connection_init/subscribe/complete + next. The handshake (connection_init/ack) is identical.
const char* GraphQlWsProtocol::tInit() const { return "connection_init"; }
const char* GraphQlWsProtocol::tSubscribe() const { return legacy() ? "start" : "subscribe"; }
const char* GraphQlWsProtocol::tComplete() const { return legacy() ? "stop" : "complete"; }

void GraphQlWsProtocol::openOnce() {
    if (opened_) return;
    opened_ = true;
    StreamMeta meta;
    meta.streamId = streamId_;
    meta.transport = StreamTransport::WebSocket;
    if (sink_) sink_->onStreamOpen(meta);
}

void GraphQlWsProtocol::closeOnce(StreamStatus status, int code, const std::string& msg) {
    if (closed_) return;
    closed_ = true;
    openOnce();   // contract: exactly one open precedes close
    StreamEnd end;
    end.status = status;
    end.statusCode = code;
    end.statusMessage = msg;
    end.totalEvents = seq_;
    if (sink_) sink_->onStreamClose(end);
}

void GraphQlWsProtocol::onOpen() {
    openOnce();
    json init;
    init["type"] = tInit();
    // The domain GraphQlRequest carries no connection_init payload (not modeled), so none is sent — matching
    // the domain send stack's behavior.
    sendRaw_(init.dump());
}

void GraphQlWsProtocol::sendSubscribe() {
    // subscribe with the operation. payload = {query, variables, operationName}.
    json sub;
    sub["id"] = id_;
    sub["type"] = tSubscribe();
    json payload;
    const auto& op = req_.op();
    payload["query"] = op.query;
    const std::string& vtxt = op.variables.text();
    try { payload["variables"] = json::parse(vtxt.empty() ? "{}" : vtxt); }
    catch (...) { payload["variables"] = json::object(); }
    if (!op.operationName.empty()) payload["operationName"] = op.operationName;
    sub["payload"] = payload;
    sendRaw_(sub.dump());
}

void GraphQlWsProtocol::emitDataPayload(const std::string& payloadJson) {
    StreamEvent ev;
    ev.seq = seq_++;
    ev.direction = StreamDirection::Inbound;
    ev.frameType = StreamFrameType::Message;
    ev.kind = StreamPayloadKind::Json;
    ev.payload = payloadJson;
    if (sink_) sink_->onStreamEvent(ev);
}

void GraphQlWsProtocol::onFrame(const std::string& text) {
    if (closed_) return;
    json msg;
    try { msg = json::parse(text); } catch (...) { return; }   // ignore non-JSON / partial
    std::string type = msg.value("type", "");
    auto payloadDump = [&](const char* def) {
        return msg.contains("payload") ? msg["payload"].dump() : std::string(def);
    };

    if (type == "connection_ack") {
        acked_ = true;
        sendSubscribe();
    } else if (type == "ping") {                // keepalive -> pong (modern); legacy "ka" needs no reply
        json pong; pong["type"] = "pong";
        sendRaw_(pong.dump());
    } else if (type == "pong" || type == "ka") {
        // no-op
    } else if (type == "next" || type == "data") {
        // A result for our subscription. payload = ExecutionResult {data,errors}.
        if (msg.value("id", "") == id_) emitDataPayload(payloadDump("null"));
    } else if (type == "error") {              // subscribe-level error: payload = [GraphQLError]
        closeOnce(StreamStatus::Error, 0, payloadDump(""));
    } else if (type == "complete") {           // server finished this subscription
        // L9: only a complete carrying OUR id closes us. A missing id is malformed per graphql-transport-ws
        // and must NOT terminate the subscription (was a premature Ok close).
        if (msg.value("id", "") == id_) closeOnce(StreamStatus::Ok, 1000, "");
    } else if (type == "connection_error") {
        closeOnce(StreamStatus::Error, 0, payloadDump(""));
    }
    // unknown type -> ignore
}

void GraphQlWsProtocol::requestStop() {
    if (closed_) return;
    json done;
    done["id"] = id_;
    done["type"] = tComplete();
    sendRaw_(done.dump());
    // The transport owner closes the socket next; onClose() will deliver onStreamClose(Cancelled/Ok).
}

void GraphQlWsProtocol::onClose(StreamStatus status, int code, const std::string& msg) {
    closeOnce(status, code, msg);
}

} // namespace core
