// infra/transport/shared/i_stream_sink.hpp — the reusable stream-receiving contract (SPEC_grpc_streaming §3).
// INV-1: UI (and any consumer) depends ONLY on this + the StreamEvent DTO — never on a transport type
// (grpc/cpr/libcurl/protobuf). A new transport = a new sender that emits StreamEvent; consumers untouched.
//
// Call contract every sender MUST honor:
//   1. exactly one onStreamOpen first,
//   2. 0..N onStreamEvent with seq 0,1,2,… (contiguous),
//   3. exactly one onStreamClose last (even on error/cancel/empty).
//   4. all callbacks of ONE stream run sequentially (sender guarantees), but may be on a background
//      thread — the consumer marshals to its UI thread itself (§6).
#pragma once

#include "infra/transport/shared/stream_events.hpp"

namespace core {

class IStreamSink {
public:
    virtual ~IStreamSink() = default;
    virtual void onStreamOpen(const StreamMeta& meta) = 0;   // pane: reset + print '['; status = streaming
    virtual void onStreamEvent(const StreamEvent& ev) = 0;   // append one JSON block
    virtual void onStreamClose(const StreamEnd& end) = 0;    // print ']'; finalize status/elapsed/count
};

} // namespace core
