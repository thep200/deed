// gql_ws_protocol_test.cpp — gatekeeper for the GraphQL-over-WS protocol layer (SPEC_graphql AC-7).
// Drives GraphQlWsProtocol with a FAKE transport (a sendRaw collector + a recording IStreamSink) and a
// canned ack/next/complete frame sequence. Includes NO libcurl/curl_ws -> proves the protocol layer is
// transport-independent (INV-1) and the §6.1 naming trap is handled.
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/graphql/gql_ws_protocol.hpp"
#include "core/streaming/i_stream_sink.hpp"
#include "core/types.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define GCHECK(cond, msg)                                                          \
    do {                                                                           \
        if (cond) { ++g_pass; }                                                    \
        else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

class RecSink : public core::IStreamSink {
public:
    int opens = 0, closes = 0;
    std::vector<std::string> events;   // payloads (ExecutionResult JSON)
    core::StreamEnd lastEnd;
    void onStreamOpen(const core::StreamMeta&) override { ++opens; }
    void onStreamEvent(const core::StreamEvent& ev) override { events.push_back(ev.payload); }
    void onStreamClose(const core::StreamEnd& end) override { ++closes; lastEnd = end; }
};

// Returns the "type" field of the last raw frame the protocol sent.
std::string typeOf(const std::string& raw) {
    try { return nlohmann::json::parse(raw).value("type", ""); } catch (...) { return ""; }
}

void test_modern_happy_path() {
    std::printf("[gql_ws: modern happy path]\n");
    RecSink sink;
    std::vector<std::string> sent;
    core::GraphQlRequest req;
    req.query = "subscription { countdown(from: 3) }";
    req.connectionInitPayloadJson = R"({"authToken":"abc"})";
    core::GraphQlWsProtocol proto("s1", req, &sink, [&](const std::string& s) { sent.push_back(s); });

    proto.onOpen();
    GCHECK(sink.opens == 1, "onStreamOpen fired once");
    GCHECK(!sent.empty() && typeOf(sent.back()) == "connection_init", "sent connection_init");
    // auth payload carried
    GCHECK(sent.back().find("authToken") != std::string::npos, "connection_init carries auth payload");

    proto.onFrame(R"({"type":"connection_ack"})");
    GCHECK(typeOf(sent.back()) == "subscribe", "ack -> sent subscribe");
    GCHECK(sent.back().find("countdown") != std::string::npos, "subscribe carries the query");

    proto.onFrame(R"({"type":"next","id":"1","payload":{"data":{"countdown":3}}})");
    proto.onFrame(R"({"type":"next","id":"1","payload":{"data":{"countdown":2}}})");
    GCHECK(sink.events.size() == 2, "two next -> two events");
    GCHECK(sink.events[0] == R"({"data":{"countdown":3}})", "event payload = ExecutionResult");

    // a next for a different id is ignored
    proto.onFrame(R"({"type":"next","id":"99","payload":{"data":{"x":1}}})");
    GCHECK(sink.events.size() == 2, "next for other id ignored");

    // ping -> pong
    size_t before = sent.size();
    proto.onFrame(R"({"type":"ping"})");
    GCHECK(sent.size() == before + 1 && typeOf(sent.back()) == "pong", "ping -> pong");

    proto.onFrame(R"({"type":"complete","id":"1"})");
    GCHECK(sink.closes == 1 && sink.lastEnd.status == core::StreamStatus::Ok, "complete -> close(Ok)");
    GCHECK(sink.lastEnd.totalEvents == 2, "totalEvents = 2");

    // frames after close are ignored
    proto.onFrame(R"({"type":"next","id":"1","payload":{"data":{}}})");
    GCHECK(sink.events.size() == 2, "no events after close");
}

void test_subscribe_error() {
    std::printf("[gql_ws: error]\n");
    RecSink sink;
    std::vector<std::string> sent;
    core::GraphQlRequest req;
    req.query = "subscription { bad }";
    core::GraphQlWsProtocol proto("s2", req, &sink, [&](const std::string& s) { sent.push_back(s); });
    proto.onOpen();
    proto.onFrame(R"({"type":"connection_ack"})");
    proto.onFrame(R"({"type":"error","id":"1","payload":[{"message":"boom"}]})");
    GCHECK(sink.closes == 1 && sink.lastEnd.status == core::StreamStatus::Error, "error -> close(Error)");
    GCHECK(sink.lastEnd.statusMessage.find("boom") != std::string::npos, "error detail carried");
}

void test_stop_sends_complete() {
    std::printf("[gql_ws: stop]\n");
    RecSink sink;
    std::vector<std::string> sent;
    core::GraphQlRequest req;
    req.query = "subscription { t }";
    core::GraphQlWsProtocol proto("s3", req, &sink, [&](const std::string& s) { sent.push_back(s); });
    proto.onOpen();
    proto.onFrame(R"({"type":"connection_ack"})");
    proto.requestStop();
    GCHECK(typeOf(sent.back()) == "complete", "Stop -> sent complete");
    proto.onClose(core::StreamStatus::Cancelled, 1000, "");
    GCHECK(sink.closes == 1 && sink.lastEnd.status == core::StreamStatus::Cancelled, "transport close -> close(Cancelled)");
}

void test_legacy_names() {
    std::printf("[gql_ws: legacy subscriptions-transport-ws]\n");
    RecSink sink;
    std::vector<std::string> sent;
    core::GraphQlRequest req;
    req.query = "subscription { t }";
    req.wsProtocol = core::GqlWsProtocol::SubscriptionsTransportWs;
    core::GraphQlWsProtocol proto("s4", req, &sink, [&](const std::string& s) { sent.push_back(s); });
    proto.onOpen();
    proto.onFrame(R"({"type":"connection_ack"})");
    GCHECK(typeOf(sent.back()) == "start", "legacy: ack -> 'start' (not subscribe)");
    proto.onFrame(R"({"type":"data","id":"1","payload":{"data":{"t":1}}})");
    GCHECK(sink.events.size() == 1, "legacy: 'data' -> event");
    proto.requestStop();
    GCHECK(typeOf(sent.back()) == "stop", "legacy: stop -> 'stop'");
}

} // namespace

// Called from test_main.cpp. Returns the number of failed checks.
int run_gql_ws_protocol_tests() {
    test_modern_happy_path();
    test_subscribe_error();
    test_stop_sends_complete();
    test_legacy_names();
    std::printf("[gql_ws_protocol] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
