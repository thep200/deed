// CoreBridge — connects Core (C++) to AppKit, honoring the threading contract (UI spec §3).
// IUiDelegate callbacks run on a BACKGROUND THREAD -> marshal back to the main queue via GCD; the UI
// checks the handle is still valid (generation) before touching views; unknown-handle callbacks -> drop silently.
//
// Streaming (SPEC_grpc_streaming §6/§8): onStreamEvent can fire VERY fast (e.g. Fibonacci). Appending one
// Scintilla mutation per event would jam the main queue, so the bridge COALESCES: events accumulate into
// a locked buffer on the background thread, and a single flush is scheduled on main every ~50ms. The
// consumer (controller) only ever sees neutral DTO-derived data — never a transport type (INV-1).
#import <Cocoa/Cocoa.h>

#include <memory>
#include <mutex>
#include <string>

#include "core/engine.hpp"
#include "core/types.hpp"

// Protocol implemented by MainWindowController (already on the main thread when called).
@protocol CoreResponseSink <NSObject>
- (void)onCoreResponse:(uint64_t)handle response:(const core::ApiResponse &)resp;
- (void)onCoreError:(uint64_t)handle error:(const core::ApiError &)err;
// --- streaming (all delivered on the main thread) ---
- (void)onStreamOpenTransport:(int)transport;                          // reset pane, print '['
- (void)onStreamChunk:(NSString *)chunk events:(uint64_t)totalEvents;  // coalesced append
- (void)onStreamClose:(core::StreamStatus)status
                 code:(int)code
              message:(NSString *)message
               events:(uint64_t)events
            elapsedMs:(long long)elapsedMs
            truncated:(BOOL)truncated;
@end

// Bridge lives alongside the controller; holds a weak back-pointer to avoid a retain cycle.
class UiDelegateBridge : public core::IUiDelegate {
public:
    // coalesceMs: UI flush cadence (STREAM_COALESCE_MS, §9). <=0 -> 50ms default.
    explicit UiDelegateBridge(id<CoreResponseSink> sink, int coalesceMs = 50)
        : sink_(sink), coalesceMs_(coalesceMs > 0 ? coalesceMs : 50),
          coal_(std::make_shared<Coalescer>()) {}

    void onResponse(core::RequestHandle h, const core::ApiResponse &r) override {
        core::ApiResponse copy = r;            // copy the neutral POD off the background thread
        __weak id<CoreResponseSink> weakSink = sink_;
        dispatch_async(dispatch_get_main_queue(), ^{
            id<CoreResponseSink> s = weakSink;
            if (s) [s onCoreResponse:h response:copy];
        });
    }
    void onError(core::RequestHandle h, const core::ApiError &e) override {
        core::ApiError copy = e;
        __weak id<CoreResponseSink> weakSink = sink_;
        dispatch_async(dispatch_get_main_queue(), ^{
            id<CoreResponseSink> s = weakSink;
            if (s) [s onCoreError:h error:copy];
        });
    }

    // --- IStreamSink (SPEC_grpc_streaming §3) ---
    void onStreamOpen(const core::StreamMeta &meta) override {
        {
            std::lock_guard<std::mutex> lk(coal_->mu);
            coal_->buf.clear();
            coal_->firstEvent = true;
            coal_->events = 0;
            coal_->flushScheduled = false;
        }
        int transport = static_cast<int>(meta.transport);
        __weak id<CoreResponseSink> weakSink = sink_;
        dispatch_async(dispatch_get_main_queue(), ^{
            id<CoreResponseSink> s = weakSink;
            if (s) [s onStreamOpenTransport:transport];
        });
    }

    void onStreamEvent(const core::StreamEvent &ev) override {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lk(coal_->mu);
            coal_->buf += (coal_->firstEvent ? "\n  " : ",\n  ");  // array comma rule (Appendix A)
            coal_->buf += ev.payload;
            coal_->firstEvent = false;
            coal_->events += 1;
            if (!coal_->flushScheduled) { coal_->flushScheduled = true; schedule = true; }
        }
        if (schedule) scheduleFlush();
    }

    void onStreamClose(const core::StreamEnd &end) override {
        auto coal = coal_;
        __weak id<CoreResponseSink> weakSink = sink_;
        core::StreamStatus status = end.status;
        int code = end.statusCode;
        std::string msg = end.statusMessage;
        uint64_t events = end.totalEvents;
        long long elapsed = end.elapsedMs;
        BOOL truncated = end.truncated ? YES : NO;
        dispatch_async(dispatch_get_main_queue(), ^{
            id<CoreResponseSink> s = weakSink;
            if (!s) return;
            std::string chunk;
            uint64_t cnt = 0;
            {
                std::lock_guard<std::mutex> lk(coal->mu);
                chunk.swap(coal->buf);     // drain whatever is still pending
                cnt = coal->events;
                coal->flushScheduled = false;
            }
            if (!chunk.empty())
                [s onStreamChunk:[NSString stringWithUTF8String:chunk.c_str()] events:cnt];
            [s onStreamClose:status
                        code:code
                     message:[NSString stringWithUTF8String:msg.c_str()]
                      events:events
                   elapsedMs:elapsed
                   truncated:truncated];
        });
    }

private:
    // Coalescing state shared between the background producer and the main-queue flush block.
    struct Coalescer {
        std::mutex mu;
        std::string buf;
        bool firstEvent = true;
        bool flushScheduled = false;
        uint64_t events = 0;
    };

    void scheduleFlush() {
        auto coal = coal_;
        __weak id<CoreResponseSink> weakSink = sink_;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(coalesceMs_ * NSEC_PER_MSEC)),
                       dispatch_get_main_queue(), ^{
            std::string chunk;
            uint64_t cnt = 0;
            {
                std::lock_guard<std::mutex> lk(coal->mu);
                chunk.swap(coal->buf);
                cnt = coal->events;
                coal->flushScheduled = false;
            }
            id<CoreResponseSink> s = weakSink;
            if (s && !chunk.empty())
                [s onStreamChunk:[NSString stringWithUTF8String:chunk.c_str()] events:cnt];
        });
    }

    __weak id<CoreResponseSink> sink_;
    int coalesceMs_;
    std::shared_ptr<Coalescer> coal_;
};
