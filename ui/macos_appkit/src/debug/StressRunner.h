// Debug-only stress driver (DEED_DEBUG_TOOLS + env DEED_STRESS=1): runs on the MAIN thread via a timer,
// randomly exercising the first-responder/Scintilla/window lifecycle paths that caused crashes.
#import <Cocoa/Cocoa.h>

#if DEED_DEBUG_TOOLS

@class MainWindowController;

@interface StressRunner : NSObject

// True when env DEED_STRESS=1. AppController calls this to decide whether to run the runner.
+ (BOOL)enabledFromEnv;

- (instancetype)initWithController:(MainWindowController *)wc;

// Reads DEED_STRESS_ITERS / DEED_STRESS_SEED / DEED_STRESS_LOG / DEED_STRESS_IDLE_EVERY from env,
// bootstraps a temp collection then starts the loop. Stops automatically once the iteration count is reached.
- (void)start;

@end

#endif // DEED_DEBUG_TOOLS
