#import "widgets/OS9Divider.h"
#import "theme/OS9Theme.h"

@implementation OS9Divider
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    [[OS9Theme face] set];
    NSRectFill(self.bounds);
    // vạch chìm giữa cho dễ thấy chỗ kéo
    CGFloat mx = floor(self.bounds.size.width / 2);
    [[OS9Theme shadow] set];
    NSRectFill(NSMakeRect(mx - 1, 2, 1, self.bounds.size.height - 4));
    [[OS9Theme highlight] set];
    NSRectFill(NSMakeRect(mx, 2, 1, self.bounds.size.height - 4));
}
- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:[NSCursor resizeLeftRightCursor]];
}
- (void)mouseDown:(NSEvent *)e {
    NSPoint last = [NSEvent mouseLocation];
    for (;;) {
        NSEvent *ev = [self.window nextEventMatchingMask:(NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp)];
        if (ev.type == NSEventTypeLeftMouseUp) break;
        NSPoint now = [NSEvent mouseLocation];
        CGFloat dx = now.x - last.x;
        last = now;
        if (self.onDrag && dx != 0) self.onDrag(dx);
    }
}
@end
