// ws_session_test.cpp — gatekeeper for INV-1 on the duplex (send+recv) contract (SPEC_websocket AC-6/AC-7).
//
// This TU includes ONLY the neutral contracts + DTOs — never a transport header (curl/curl_ws). If a
// future change leaks a transport type into the consumer side, this stops compiling. The live echo
// (AC-1/AC-2) runs via the CLI: `apicli ws wss://ws.postman-echo.com/raw <msg>`.
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "core/streaming/i_stream_channel.hpp"
#include "core/streaming/i_stream_sink.hpp"
#include "core/types.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define WCHECK(cond, msg)                                                          \
    do {                                                                           \
        if (cond) { ++g_pass; }                                                    \
        else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

// A consumer written against the contract alone — records the duplex session.
class RecordingSink : public core::IStreamSink {
public:
    int opens = 0, closes = 0, inbound = 0, outbound = 0;
    core::StreamEnd lastEnd;
    void onStreamOpen(const core::StreamMeta&) override { ++opens; }
    void onStreamEvent(const core::StreamEvent& ev) override {
        if (ev.direction == core::StreamDirection::Outbound) ++outbound;
        else ++inbound;
    }
    void onStreamClose(const core::StreamEnd& end) override { ++closes; lastEnd = end; }
};

// A transport-free channel with a bounded send queue (mimics WsChannel backpressure semantics).
class FakeChannel : public core::IStreamChannel {
public:
    std::deque<std::string> queue;
    std::size_t maxFrames = 4;
    bool open = true;
    bool sendText(const std::string& s) override {
        if (!open || queue.size() >= maxFrames) return false;   // backpressure (§11)
        queue.push_back(s);
        return true;
    }
    bool sendBinary(const std::vector<std::uint8_t>& b) override {
        if (!open || queue.size() >= maxFrames) return false;
        queue.emplace_back(b.begin(), b.end());
        return true;
    }
    void close(int, const std::string&) override { open = false; }
    bool isOpen() const override { return open; }
};

// Transport-free duplex driver: open -> (consume the channel's queued sends as Outbound, echo each back
// as Inbound) -> close. Stands in for WsSender to prove the contract is enough (no curl).
void fakeDuplexEcho(core::IStreamSink& sink, FakeChannel& ch) {
    core::StreamMeta meta;
    meta.streamId = "fake-ws";
    meta.transport = core::StreamTransport::WebSocket;
    sink.onStreamOpen(meta);

    std::uint64_t seq = 0;
    while (!ch.queue.empty()) {
        std::string body = ch.queue.front();
        ch.queue.pop_front();
        core::StreamEvent out;
        out.seq = seq++;
        out.direction = core::StreamDirection::Outbound;
        out.frameType = core::StreamFrameType::Text;
        out.payload = body;
        sink.onStreamEvent(out);

        core::StreamEvent in;
        in.seq = seq++;
        in.direction = core::StreamDirection::Inbound;
        in.frameType = core::StreamFrameType::Text;
        in.payload = body;   // echo
        sink.onStreamEvent(in);
    }

    core::StreamEnd end;
    end.status = core::StreamStatus::Ok;
    end.statusCode = 1000;
    end.totalEvents = seq;
    sink.onStreamClose(end);
}

void test_duplex_reuse() {
    std::printf("[ws_session: duplex reuse (AC-6)]\n");
    RecordingSink sink;
    FakeChannel ch;
    WCHECK(ch.isOpen(), "channel open before session");
    WCHECK(ch.sendText("a"), "sendText queued");
    WCHECK(ch.sendBinary({1, 2, 3}), "sendBinary queued");

    fakeDuplexEcho(sink, ch);

    WCHECK(sink.opens == 1 && sink.closes == 1, "exactly one open + one close");
    WCHECK(sink.outbound == 2, "2 outbound frames logged");
    WCHECK(sink.inbound == 2, "2 inbound (echo) frames logged");
    WCHECK(sink.lastEnd.statusCode == 1000, "close code 1000");
}

void test_backpressure() {
    std::printf("[ws_session: backpressure (AC-7)]\n");
    FakeChannel ch;
    ch.maxFrames = 4;
    int ok = 0;
    bool refusedWhenFull = false;
    for (int i = 0; i < 10; ++i) {
        if (ch.sendText("x")) ++ok;
        else { refusedWhenFull = true; break; }
    }
    WCHECK(ok == 4, "accepts up to the queue cap");
    WCHECK(refusedWhenFull, "sendText returns false when the queue is full (no unbounded growth)");

    ch.close(1000, "");
    WCHECK(!ch.isOpen(), "closed channel reports not open");
    WCHECK(!ch.sendText("y"), "send after close is refused");
}

} // namespace

// Called from test_main.cpp. Returns the number of failed checks.
int run_ws_session_tests() {
    test_duplex_reuse();
    test_backpressure();
    std::printf("[ws_session] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
