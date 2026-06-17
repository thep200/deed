#import <Cocoa/Cocoa.h>

#import "CoreBridge.h"

// Cửa sổ chính: cây (1)(2), editor JSON-thô (3), response (4), ENV (5), method (6),
// URL/target (7), status (8), proto (9), send (10), setting (11). Xem UI spec §5.
@interface MainWindowController : NSObject <CoreResponseSink,
                                            NSOutlineViewDataSource,
                                            NSOutlineViewDelegate,
                                            NSTextViewDelegate,
                                            NSTextFieldDelegate,
                                            NSWindowDelegate>
- (void)showWindow;
- (void)openCollectionRoot:(NSString *)path; // mở collection theo path (dev/CI affordance)

// Action (cũng được gọi từ menu qua AppController).
- (void)openFolder:(id)sender;
- (void)saveRequest:(id)sender;
- (void)sendRequest:(id)sender;
- (void)copyAsCurl:(id)sender;
@end
