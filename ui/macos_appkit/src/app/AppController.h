#import <Cocoa/Cocoa.h>

@class MainWindowController;

// App delegate: tạo cửa sổ chính, chuyển tiếp các action menu.
@interface AppController : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) MainWindowController *mainWC;
- (void)openFolder:(id)sender;
- (void)saveRequest:(id)sender;
- (void)sendRequest:(id)sender;
@end
