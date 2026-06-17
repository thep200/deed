#import "widgets/OS9BackgroundView.h"
#import "theme/OS9Theme.h"

@implementation OS9BackgroundView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    [[OS9Theme face] set];
    NSRectFill(self.bounds);
}
@end
