#import <Cocoa/Cocoa.h>

@class MainWindowController;

// App delegate: creates the main window, forwards menu actions.
@interface AppController : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) MainWindowController *mainWC;
- (void)openFolder:(id)sender;
- (void)saveRequest:(id)sender;
- (void)sendRequest:(id)sender;
@end
