// Consumers depend only on this + StreamEvent — never on a transport type.
// Contract: exactly one onStreamOpen first; 0..N onStreamEvent with contiguous seq 0,1,2,…; exactly one
// onStreamClose last (even on error/cancel/empty). Callbacks of one stream run sequentially, but may be
// on a background thread — the consumer marshals to its UI thread itself.
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
