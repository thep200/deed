// gql_ws_protocol.cpp — graphql-transport-ws / legacy graphql-ws protocol state machine (SPEC_graphql §6).
#include "core/graphql/gql_ws_protocol.hpp"

#include <utility>

#include <nlohmann/json.hpp>

namespace core {

using nlohmann::json;

GraphQlWsProtocol::GraphQlWsProtocol(std::string streamId, GraphQlRequest req, IStreamSink* uiSink,
                                     std::function<void(const std::string&)> sendRaw)
    : streamId_(std::move(streamId)), req_(std::move(req)), sink_(uiSink), sendRaw_(std::move(sendRaw)) {}

bool GraphQlWsProtocol::legacy() const {
    return req_.wsProtocol == GqlWsProtocol::SubscriptionsTransportWs;
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
    if (!req_.connectionInitPayloadJson.empty()) {
        try { init["payload"] = json::parse(req_.connectionInitPayloadJson); } catch (...) {}
    }
    sendRaw_(init.dump());
}

void GraphQlWsProtocol::onFrame(const std::string& text) {
    if (closed_) return;
    json msg;
    try { msg = json::parse(text); } catch (...) { return; }   // ignore non-JSON / partial
    std::string type = msg.value("type", "");

    if (type == "connection_ack") {
        acked_ = true;
        // subscribe with the operation. payload = {query, variables, operationName}.
        json sub;
        sub["id"] = id_;
        sub["type"] = tSubscribe();
        json payload;
        payload["query"] = req_.query;
        try { payload["variables"] = json::parse(req_.variablesJson.empty() ? "{}" : req_.variablesJson); }
        catch (...) { payload["variables"] = json::object(); }
        if (!req_.operationName.empty()) payload["operationName"] = req_.operationName;
        sub["payload"] = payload;
        sendRaw_(sub.dump());
        return;
    }
    if (type == "ping") {                       // keepalive -> pong (modern); legacy "ka" needs no reply
        json pong; pong["type"] = "pong";
        sendRaw_(pong.dump());
        return;
    }
    if (type == "pong" || type == "ka") return;

    // A result for our subscription: modern "next" | legacy "data". payload = ExecutionResult {data,errors}.
    if (type == "next" || type == "data") {
        if (msg.value("id", "") != id_) return;
        std::string payload = msg.contains("payload") ? msg["payload"].dump() : std::string("null");
        StreamEvent ev;
        ev.seq = seq_++;
        ev.direction = StreamDirection::Inbound;
        ev.frameType = StreamFrameType::Message;
        ev.kind = StreamPayloadKind::Json;
        ev.payload = payload;
        if (sink_) sink_->onStreamEvent(ev);
        return;
    }
    if (type == "error") {                      // subscribe-level error: payload = [GraphQLError]
        std::string detail = msg.contains("payload") ? msg["payload"].dump() : std::string();
        closeOnce(StreamStatus::Error, 0, detail);
        return;
    }
    if (type == "complete") {                   // server finished this subscription
        // L9: only a complete carrying OUR id closes us. A missing id is malformed per graphql-transport-ws
        // and must NOT terminate the subscription (was a premature Ok close).
        if (msg.value("id", "") != id_) return;
        closeOnce(StreamStatus::Ok, 1000, "");
        return;
    }
    if (type == "connection_error") {
        std::string detail = msg.contains("payload") ? msg["payload"].dump() : std::string();
        closeOnce(StreamStatus::Error, 0, detail);
        return;
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
