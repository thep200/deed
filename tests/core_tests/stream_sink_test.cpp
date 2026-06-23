// stream_sink_test.cpp — gatekeeper for INV-1 (SPEC_grpc_streaming §12 AC-4, Appendix B).
//
// This translation unit includes ONLY the neutral stream contract + DTOs. It MUST NOT include any
// transport header (grpc/grpcpp, protobuf, cpr, libcurl). If a future change leaks such a type into
// the consumer side of the contract, this file stops compiling — that is the point.
//
// AC-1..3/AC-5 need a live server (Calc/fibonacci @ localhost:8765) and are exercised manually via the
// CLI (`apicli send <root> <rel>`). Here we prove the CONTRACT + ordering with a transport-free producer:
// a consumer written against IStreamSink alone receives open -> N×event(seq 0..N-1) -> close exactly once.
#include <cstdio>
#include <string>
#include <vector>

#include "core/streaming/i_stream_sink.hpp"
#include "core/types.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define SCHECK(cond, msg)                                                         \
    do {                                                                          \
        if (cond) { ++g_pass; }                                                   \
        else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

// A consumer that depends on NOTHING but the contract — it records the call sequence to assert on.
// This is exactly what a CLI harness, a cache writer, or the UI bridge is (minus the marshalling).
class RecordingSink : public core::IStreamSink {
public:
    int opens = 0;
    int closes = 0;
    std::vector<std::uint64_t> seqs;       // every event's seq, in arrival order
    std::string assembled;                 // the array form, assembled by the consumer (Appendix A)
    core::StreamEnd lastEnd;
    bool sawEventAfterClose = false;
    bool sawEventBeforeOpen = false;

    void onStreamOpen(const core::StreamMeta&) override {
        ++opens;
        assembled = "[";
    }
    void onStreamEvent(const core::StreamEvent& ev) override {
        if (opens == 0) sawEventBeforeOpen = true;
        if (closes > 0) sawEventAfterClose = true;
        assembled += (ev.seq == 0 ? "\n  " : ",\n  ") + ev.payload;
        seqs.push_back(ev.seq);
    }
    void onStreamClose(const core::StreamEnd& end) override {
        ++closes;
        assembled += seqs.empty() ? "]" : "\n]";
        lastEnd = end;
    }
};

// Transport-free producer: emits the §3 sequence (open -> n events -> close). Stands in for any sender.
void fakeServerStream(core::IStreamSink& sink, int n, core::StreamStatus status, bool truncated) {
    core::StreamMeta meta;
    meta.streamId = "fake-1";
    meta.transport = core::StreamTransport::Sse;   // deliberately NOT Grpc — consumer must not care (INV-1)
    sink.onStreamOpen(meta);

    std::uint64_t bytes = 0;
    for (int i = 0; i < n; ++i) {
        core::StreamEvent ev;
        ev.seq = static_cast<std::uint64_t>(i);
        ev.kind = core::StreamPayloadKind::Json;
        ev.payload = "{\"fib\":" + std::to_string(i) + "}";
        ev.name = "message";
        ev.offsetMs = i;
        sink.onStreamEvent(ev);
        bytes += ev.payload.size();
    }

    core::StreamEnd end;
    end.status = status;
    end.totalEvents = static_cast<std::uint64_t>(n);
    end.totalBytes = bytes;
    end.elapsedMs = n;
    end.truncated = truncated;
    sink.onStreamClose(end);
}

void test_happy_path() {
    std::printf("[stream_sink: happy path]\n");
    RecordingSink sink;
    fakeServerStream(sink, 5, core::StreamStatus::Ok, false);

    SCHECK(sink.opens == 1, "exactly one onStreamOpen");
    SCHECK(sink.closes == 1, "exactly one onStreamClose");
    SCHECK(!sink.sawEventBeforeOpen, "no event before open");
    SCHECK(!sink.sawEventAfterClose, "no event after close");
    SCHECK(sink.seqs.size() == 5, "received 5 events");

    bool contiguous = true;
    for (std::uint64_t i = 0; i < sink.seqs.size(); ++i)
        if (sink.seqs[i] != i) contiguous = false;
    SCHECK(contiguous, "seq is contiguous 0..N-1");
    SCHECK(sink.lastEnd.status == core::StreamStatus::Ok, "close status Ok");
    SCHECK(sink.lastEnd.totalEvents == 5, "totalEvents = 5");
    SCHECK(sink.assembled == "[\n  {\"fib\":0},\n  {\"fib\":1},\n  {\"fib\":2},\n  {\"fib\":3},\n  {\"fib\":4}\n]",
           "assembled array is valid (Appendix A layout)");
}

void test_empty_stream() {
    std::printf("[stream_sink: empty]\n");
    RecordingSink sink;
    fakeServerStream(sink, 0, core::StreamStatus::Ok, false);
    SCHECK(sink.opens == 1 && sink.closes == 1, "open+close even when empty");
    SCHECK(sink.seqs.empty(), "no events");
    SCHECK(sink.assembled == "[]", "empty stream -> []");
}

void test_cancelled_partial() {
    std::printf("[stream_sink: cancelled partial]\n");
    RecordingSink sink;
    fakeServerStream(sink, 3, core::StreamStatus::Cancelled, false);
    SCHECK(sink.closes == 1 && sink.lastEnd.status == core::StreamStatus::Cancelled, "close(Cancelled)");
    SCHECK(sink.seqs.size() == 3, "partial events kept");
    SCHECK(sink.assembled.front() == '[' && sink.assembled.back() == ']', "array still closed validly");
}

} // namespace

// Called from test_main.cpp. Returns the number of failed checks (0 = all passed).
int run_stream_sink_tests() {
    test_happy_path();
    test_empty_stream();
    test_cancelled_partial();
    std::printf("[stream_sink] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
