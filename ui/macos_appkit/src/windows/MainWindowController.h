#import <Cocoa/Cocoa.h>

#import "bridge/CoreBridge.h"

// Protocol conformances are declared on the category that implements them (see Private.h); the primary
// keeps only all-optional NSWindowDelegate -> avoids -Wincomplete-implementation warnings.
@interface MainWindowController : NSObject <NSWindowDelegate>
- (void)showWindow;
@end

// Menu/AppController actions. Implemented in categories — declared outside the primary interface so
// clang doesn't require them in the main @implementation.
@interface MainWindowController (Actions)
- (void)flushCaches;   // persist deferred cache metadata before the app exits
- (void)openCollectionRoot:(NSString *)path; // open collection at path (dev/CI affordance)
- (void)openFolder:(id)sender;
- (void)saveRequest:(id)sender;
- (void)sendRequest:(id)sender;
- (void)copyAsCurl:(id)sender;
@end
