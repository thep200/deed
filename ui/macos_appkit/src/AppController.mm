#import "AppController.h"
#import "MainWindowController.h"

@implementation AppController

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    self.mainWC = [MainWindowController new];
    [self.mainWC showWindow];

    // Dev/CI: APICLIENT_OPEN=<path> -> mở sẵn collection lúc khởi động (bỏ qua file picker).
    const char *autoOpen = getenv("APICLIENT_OPEN");
    if (autoOpen && *autoOpen) {
        [self.mainWC openCollectionRoot:[NSString stringWithUTF8String:autoOpen]];
    }
    // CI smoke: APICLIENT_SEND=1 -> tự bấm Send sau khi nạp (kiểm full luồng UI→Core→UI).
    if (getenv("APICLIENT_SEND")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            [self.mainWC sendRequest:nil];
        });
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

- (void)openFolder:(id)sender  { [self.mainWC openFolder:sender]; }
- (void)saveRequest:(id)sender { [self.mainWC saveRequest:sender]; }
- (void)sendRequest:(id)sender { [self.mainWC sendRequest:sender]; }

@end
