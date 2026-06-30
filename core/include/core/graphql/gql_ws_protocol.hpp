// core/graphql/gql_ws_protocol.hpp — the GraphQL-over-WebSocket protocol layer (SPEC_graphql §6).
// PURE: it speaks `graphql-transport-ws` (modern) / `graphql-ws` legacy purely in terms of raw text frames
// in (onFrame) and a sendRaw callback out — NO libcurl/curl_ws. It translates the connection_init/ack/
// subscribe/next/error/complete envelope into clean StreamEvents on a UI IStreamSink, so the UI sees a
// plain server-stream (INV-1). Reusable + unit-testable with a fake transport (AC-7).
//
// §6.1 naming trap: subprotocol `graphql-transport-ws` <-> library graphql-ws (modern, default);
//                   subprotocol `graphql-ws`           <-> library subscriptions-transport-ws (legacy).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/domain/graphql/graphql_request.hpp"
#include "core/streaming/i_stream_sink.hpp"

namespace core {

class GraphQlWsProtocol {
public:
    // uiSink receives the cleaned StreamEvents/open/close; sendRaw sends a raw WS text frame to the server.
    GraphQlWsProtocol(std::string streamId, core::domain::GraphQlRequest req, IStreamSink* uiSink,
                      std::function<void(const std::string&)> sendRaw);

    void onOpen();                          // transport connected -> send connection_init (+ onStreamOpen)
    void onFrame(const std::string& text);  // a raw WS text frame -> parse + react
    void onClose(StreamStatus status, int code, const std::string& msg);  // transport closed -> onStreamClose
    void requestStop();                     // user Stop -> send complete{id}

    bool legacy() const;                     // true = subscriptions-transport-ws message names
    bool closed() const { return closed_; }  // true once a terminal close was emitted (transport may stop)

private:
    void openOnce();
    void closeOnce(StreamStatus status, int code, const std::string& msg);
    void sendSubscribe();                    // on connection_ack: send subscribe/start{query,variables}
    void emitDataPayload(const std::string& payloadJson); // on next/data: forward one StreamEvent
    const char* tInit() const;      // connection_init
    const char* tSubscribe() const; // subscribe | start
    const char* tComplete() const;  // complete | stop

    std::string streamId_;
    core::domain::GraphQlRequest req_;
    IStreamSink* sink_;
    std::function<void(const std::string&)> sendRaw_;
    std::string id_ = "1";          // subscription id (single subscription per session in v1)
    std::uint64_t seq_ = 0;
    bool opened_ = false;
    bool closed_ = false;
    bool acked_ = false;
};

} // namespace core
