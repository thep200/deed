#import "widgets/OS9Scroller.h"
#import "theme/OS9Theme.h"

@implementation OS9Scroller

// Overlay: auto-hides, shows only while scrolling.
+ (BOOL)isCompatibleWithOverlayScrollers { return YES; }

// Fixed 16px width to fully fit the 3 center ribs (per scrollbar.svg).
+ (CGFloat)scrollerWidthForControlSize:(NSControlSize)cs scrollerStyle:(NSScrollerStyle)st {
    return 16;
}

// Transparent track (matches pane background) — draw nothing.
- (void)drawKnobSlotInRect:(NSRect)slot highlight:(BOOL)flag {}

// GRAY thumb per scrollbar.svg: gray fill + #262626 border + center ribs. Track hidden, auto-hides when not scrolling.
- (void)drawKnob {
    NSRect k = [self rectForPart:NSScrollerKnob];
    if (NSIsEmptyRect(k)) return;
    BOOL vert = (self.bounds.size.height >= self.bounds.size.width);
    // Use the scroller's FULL WIDTH (ignore the "thin" width when overlay is not hovered);
    // take only knob position/length along the scroll axis.
    NSRect b = self.bounds;
    NSRect kk = vert ? NSMakeRect(b.origin.x + 1, k.origin.y, b.size.width - 2, k.size.height)
                     : NSMakeRect(k.origin.x, b.origin.y + 1, k.size.width, b.size.height - 2);
    [[OS9Theme scrollerThumb] set];  // gray (#999999)
    NSRectFill(kk);
    [[OS9Theme scrollerBorder] set]; // border #262626
    NSFrameRect(kk);
    // 3 center ribs (dark line + white highlight), fully within thumb width
    CGFloat cx = NSMidX(kk), cy = NSMidY(kk);
    CGFloat gw = vert ? (kk.size.width - 6) : 6;   // rib length
    CGFloat gh = vert ? 6 : (kk.size.height - 6);
    if (gw < 4) gw = 4; if (gh < 4) gh = 4;
    for (int i = -1; i <= 1; i++) {                // 3 lines
        if (vert) {
            CGFloat y = floor(cy + i * 3);
            [[OS9Theme scrollerGripDark] set];  NSRectFill(NSMakeRect(floor(cx - gw / 2), y, gw, 1));
            [[OS9Theme scrollerGripLight] set]; NSRectFill(NSMakeRect(floor(cx - gw / 2), y + 1, gw, 1));
        } else {
            CGFloat x = floor(cx + i * 3);
            [[OS9Theme scrollerGripDark] set];  NSRectFill(NSMakeRect(x, floor(cy - gh / 2), 1, gh));
            [[OS9Theme scrollerGripLight] set]; NSRectFill(NSMakeRect(x + 1, floor(cy - gh / 2), 1, gh));
        }
    }
}

@end
