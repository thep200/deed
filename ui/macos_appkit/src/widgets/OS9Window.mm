#import "widgets/OS9Window.h"

@implementation OS9Window
// Borderless windows are NOT allowed to be key/main by default -> force-allow so they can take keystrokes/activate.
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end
