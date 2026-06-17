#import "widgets/OS9InsetView.h"
#import "theme/OS9Theme.h"

@implementation OS9InsetView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    [[OS9Theme face] set];
    NSRectFill(self.bounds);
    [OS9Theme drawInsetInRect:self.bounds];
}
@end
