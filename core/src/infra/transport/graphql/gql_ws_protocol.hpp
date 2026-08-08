// Pure protocol layer: raw text frames in (onFrame), a sendRaw callback out — no libcurl/curl_ws, so it
// unit-tests with a fake transport. Naming trap: subprotocol `graphql-transport-ws` <-> library graphql-ws
// (modern, default); subprotocol `graphql-ws` <-> library subscriptions-transport-ws (legacy).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/domain/graphql/graphql_request.hpp"
#include "infra/transport/shared/i_stream_sink.hpp"

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
    void emitDataPayload(std::string payloadJson); // on next/data: forward one StreamEvent (moved in)
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
