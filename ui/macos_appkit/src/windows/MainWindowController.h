#import <Cocoa/Cocoa.h>

#import "bridge/CoreBridge.h"

// Main window: tree (1)(2), raw-JSON editor (3), response (4), ENV (5), method (6),
// URL/target (7), status (8), proto (9), send (10), setting (11). See UI spec §5.
// Controller split into several category files (+Tree/+Editor/+Send/+Config/+Stress) for brevity.
// Conformance protocols live in the category that implements them (declared in MainWindowControllerPrivate.h):
//   NSOutlineView* -> +Tree, NSTextField/TextView -> +Editor, CoreResponseSink -> +Send.
// Primary keeps only NSWindowDelegate (implemented in MainWindowController.mm) -> avoids
// -Wincomplete-implementation / -Wobjc-protocol-method-implementation warnings.
@interface MainWindowController : NSObject <NSWindowDelegate>
- (void)showWindow;
- (void)flushCaches;   // persist deferred cache metadata before the app exits (Fix 2)
@end

// Public actions (called from AppController/menu). Implemented in a category, so declared
// separately outside the primary interface so clang doesn't require them in the main @implementation.
@interface MainWindowController (Actions)
- (void)openCollectionRoot:(NSString *)path; // open collection at path (dev/CI affordance)
- (void)openFolder:(id)sender;
- (void)saveRequest:(id)sender;
- (void)sendRequest:(id)sender;
- (void)copyAsCurl:(id)sender;
@end
