// UiObserver marshals/coalesces domain ResponseEvents onto the main queue and calls this sink.
#import <Cocoa/Cocoa.h>

#include <cstdint>

#include "core/domain/response/api_error.hpp"
#include "core/domain/response/api_response.hpp"
#include "core/domain/response/interaction.hpp" // StreamStatus (streaming-close enum)

// Protocol implemented by MainWindowController (already on the main thread when called).
@protocol CoreResponseSink <NSObject>
- (void)onCoreResponse:(uint64_t)handle response:(const core::domain::ApiResponse &)resp;
- (void)onCoreError:(uint64_t)handle error:(const core::domain::ApiError &)err;
// `token` identifies which stream the callback belongs to: the controller stamps each stream via
// setStreamToken and drops callbacks whose token != the active one (late callbacks from a cancelled stream).
- (void)onStreamOpenTransport:(int)transport token:(uint64_t)token;                  // reset pane, print '['
- (void)onStreamChunk:(NSString *)chunk events:(uint64_t)totalEvents token:(uint64_t)token;  // coalesced append
- (void)onStreamClose:(core::StreamStatus)status
                 code:(int)code
              message:(NSString *)message
               events:(uint64_t)events
            elapsedMs:(long long)elapsedMs
            truncated:(BOOL)truncated
                token:(uint64_t)token;
@end
