// Transport-free gatekeeper: includes ONLY the pure parser — if a transport type leaks in, this stops compiling.
#include <cstdio>
#include <string>
#include <vector>

#include "infra/transport/shared/sse_parser.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define ECHECK(cond, msg)                                                          \
    do {                                                                           \
        if (cond) { ++g_pass; }                                                    \
        else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

std::vector<core::SseEvent> drain(core::SseParser& p, const std::string& chunk) {
    std::vector<core::SseEvent> out;
    p.feed(chunk, [&](const core::SseEvent& e) { out.push_back(e); });
    return out;
}

void test_basic_and_default_event() {
    std::printf("[sse: basic]\n");
    core::SseParser p;
    auto ev = drain(p, "data: hello\n\n");
    ECHECK(ev.size() == 1, "one event");
    ECHECK(ev[0].data == "hello", "data = hello");
    ECHECK(ev[0].event.empty(), "no explicit event type (caller defaults to message)");

    auto ev2 = drain(p, "event: tick\nid: 7\ndata: {\"n\":1}\n\n");
    ECHECK(ev2.size() == 1, "second event");
    ECHECK(ev2[0].event == "tick", "event = tick");
    ECHECK(ev2[0].id == "7", "id = 7");
    ECHECK(ev2[0].data == "{\"n\":1}", "data preserved");
    ECHECK(p.lastEventId() == "7", "lastEventId tracked");
}

void test_multiline_data() {
    std::printf("[sse: multi-line data]\n");
    core::SseParser p;
    auto ev = drain(p, "data: line1\ndata: line2\n\n");
    ECHECK(ev.size() == 1, "one event from two data lines");
    ECHECK(ev[0].data == "line1\nline2", "data joined with \\n, trailing \\n dropped");
}

void test_chunk_split() {
    std::printf("[sse: chunk split]\n");
    core::SseParser p;
    std::vector<core::SseEvent> all;
    auto emit = [&](const core::SseEvent& e) { all.push_back(e); };
    p.feed("data: hel", emit);          // half a line
    ECHECK(all.empty(), "no event until line completes");
    p.feed("lo\n", emit);               // finish the data line (no blank yet)
    ECHECK(all.empty(), "no event until blank line");
    p.feed("\n", emit);                 // blank -> dispatch
    ECHECK(all.size() == 1 && all[0].data == "hello", "reassembled across chunks");
}

void test_crlf_split() {
    std::printf("[sse: CRLF split across chunks]\n");
    core::SseParser p;
    std::vector<core::SseEvent> all;
    auto emit = [&](const core::SseEvent& e) { all.push_back(e); };
    p.feed("data: x\r", emit);          // trailing CR -> must defer (could be CRLF)
    p.feed("\ndata: y\r\n\r\n", emit);  // CR completed as CRLF; then y; then blank (CRLF)
    ECHECK(all.size() == 1, "one event despite CRLF split");
    ECHECK(all[0].data == "x\ny", "CRLF handled, both lines joined");
}

void test_comment_heartbeat() {
    std::printf("[sse: comment heartbeat]\n");
    core::SseParser p;
    auto ev = drain(p, ": keep-alive\n\n");
    ECHECK(ev.empty(), "comment + blank -> no event (data buffer empty)");
    auto ev2 = drain(p, ": ping\ndata: real\n\n");
    ECHECK(ev2.size() == 1 && ev2[0].data == "real", "comment ignored, real event still dispatched");
}

void test_id_and_retry() {
    std::printf("[sse: id + retry]\n");
    core::SseParser p;
    drain(p, "retry: 4500\nid: 99\ndata: a\n\n");
    ECHECK(p.retryMs() == 4500, "retry parsed");
    ECHECK(p.lastEventId() == "99", "id parsed");
    auto ev = drain(p, "data: b\n\n");
    ECHECK(ev.size() == 1 && ev[0].id == "99", "lastEventId persists across events");
    drain(p, "id\ndata: c\n\n");
    ECHECK(p.lastEventId().empty(), "empty id sets lastEventId to \"\"");
}

void test_bom() {
    std::printf("[sse: BOM]\n");
    core::SseParser p;
    auto ev = drain(p, std::string("\xEF\xBB\xBF") + "data: z\n\n");
    ECHECK(ev.size() == 1 && ev[0].data == "z", "leading BOM stripped");
}

void test_max_bytes() {
    std::printf("[sse: max event bytes]\n");
    core::SseParser p;
    p.setMaxEventBytes(8);
    auto ev = drain(p, "data: 0123456789ABCDEF\n\n");
    ECHECK(ev.size() == 1, "event still dispatched");
    ECHECK(ev[0].data.size() <= 8, "data capped to max bytes");
    ECHECK(p.truncated(), "truncated flag set");
}

void test_finish_flush() {
    std::printf("[sse: finish flush on EOF]\n");
    core::SseParser p;
    std::vector<core::SseEvent> out;
    auto emit = [&](const core::SseEvent& e) { out.push_back(e); };
    p.feed("data: tail", emit);   // no terminating blank line -> nothing dispatched yet
    ECHECK(out.empty(), "no event before finish");
    p.finish(emit);               // clean EOF -> flush the buffered final event
    ECHECK(out.size() == 1, "finish dispatches the final event");
    ECHECK(!out.empty() && out[0].data == "tail", "final event data preserved");

    // A trailing lone '\r' deferred by feed() must also be flushed at EOF.
    core::SseParser p2;
    out.clear();
    p2.feed("data: x\r", emit);   // trailing CR deferred (could be CRLF)
    p2.finish(emit);
    ECHECK(out.size() == 1 && out[0].data == "x", "deferred-CR final event flushed");
}

} // namespace

int run_sse_parser_tests() {
    test_basic_and_default_event();
    test_multiline_data();
    test_chunk_split();
    test_crlf_split();
    test_comment_heartbeat();
    test_id_and_retry();
    test_bom();
    test_max_bytes();
    test_finish_flush();
    std::printf("[sse_parser] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
