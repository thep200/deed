// CoreBridge — nối Core (C++) với AppKit, tôn trọng hợp đồng threading (UI spec §3).
// IUiDelegate callback chạy THREAD NỀN -> marshal về main queue bằng GCD; UI kiểm tra
// handle còn hợp lệ (generation) trước khi chạm view; callback handle lạ -> drop im lặng.
#import <Cocoa/Cocoa.h>

#include "core/engine.hpp"
#include "core/types.hpp"

// Protocol mà MainWindowController implement (đã ở main thread khi được gọi).
@protocol CoreResponseSink <NSObject>
- (void)onCoreResponse:(uint64_t)handle response:(const core::ApiResponse &)resp;
- (void)onCoreError:(uint64_t)handle error:(const core::ApiError &)err;
@end

// Bridge sống cùng controller; giữ weak back-pointer để tránh retain cycle.
class UiDelegateBridge : public core::IUiDelegate {
public:
    explicit UiDelegateBridge(id<CoreResponseSink> sink) : sink_(sink) {}

    void onResponse(core::RequestHandle h, const core::ApiResponse &r) override {
        core::ApiResponse copy = r;            // copy POD trung lập ra khỏi thread nền
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
