// CoreBridge — connects Core (C++) to AppKit, honoring the threading contract (UI spec §3).
// IUiDelegate callbacks run on a BACKGROUND THREAD -> marshal back to the main queue via GCD; the UI
// checks the handle is still valid (generation) before touching views; unknown-handle callbacks -> drop silently.
#import <Cocoa/Cocoa.h>

#include "core/engine.hpp"
#include "core/types.hpp"

// Protocol implemented by MainWindowController (already on the main thread when called).
@protocol CoreResponseSink <NSObject>
- (void)onCoreResponse:(uint64_t)handle response:(const core::ApiResponse &)resp;
- (void)onCoreError:(uint64_t)handle error:(const core::ApiError &)err;
@end

// Bridge lives alongside the controller; holds a weak back-pointer to avoid a retain cycle.
class UiDelegateBridge : public core::IUiDelegate {
public:
    explicit UiDelegateBridge(id<CoreResponseSink> sink) : sink_(sink) {}

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

private:
    __weak id<CoreResponseSink> sink_;
};
