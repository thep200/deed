#import "widgets/OS9Window.h"

@implementation OS9Window
// Borderless mặc định KHÔNG được làm key/main -> ép cho phép để gõ phím/active được.
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end
