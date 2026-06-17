#import "widgets/OS9Scroller.h"

@implementation OS9Scroller

// Overlay: tự ẩn, chỉ hiện khi cuộn.
+ (BOOL)isCompatibleWithOverlayScrollers { return YES; }

// Bề rộng cố định 16px để chứa trọn 3 gân giữa thân (theo scrollbar.svg).
+ (CGFloat)scrollerWidthForControlSize:(NSControlSize)cs scrollerStyle:(NSScrollerStyle)st {
    return 16;
}

// Track trong suốt (trùng nền pane) — không vẽ gì.
- (void)drawKnobSlotInRect:(NSRect)slot highlight:(BOOL)flag {}

// Thumb XÁM kiểu scrollbar.svg: nền xám + viền #262626 + gân giữa. Track ẩn, tự ẩn khi không cuộn.
- (void)drawKnob {
    NSRect k = [self rectForPart:NSScrollerKnob];
    if (NSIsEmptyRect(k)) return;
    BOOL vert = (self.bounds.size.height >= self.bounds.size.width);
    // Dùng BỀ RỘNG ĐẦY ĐỦ của scroller (bỏ qua bề rộng "mảnh" khi overlay chưa hover);
    // chỉ lấy vị trí/độ dài knob theo trục cuộn.
    NSRect b = self.bounds;
    NSRect kk = vert ? NSMakeRect(b.origin.x + 1, k.origin.y, b.size.width - 2, k.size.height)
                     : NSMakeRect(k.origin.x, b.origin.y + 1, k.size.width, b.size.height - 2);
    [[NSColor colorWithCalibratedWhite:0.6 alpha:0.95] set]; // xám (#999999)
    NSRectFill(kk);
    [[NSColor colorWithCalibratedWhite:0.15 alpha:0.95] set]; // viền #262626
    NSFrameRect(kk);
    // 3 gân giữa thân (vạch tối + highlight trắng), bao trọn trong bề rộng thumb
    CGFloat cx = NSMidX(kk), cy = NSMidY(kk);
    CGFloat gw = vert ? (kk.size.width - 6) : 6;   // chiều dài gân
    CGFloat gh = vert ? 6 : (kk.size.height - 6);
    if (gw < 4) gw = 4; if (gh < 4) gh = 4;
    for (int i = -1; i <= 1; i++) {                // 3 gạch
        if (vert) {
            CGFloat y = floor(cy + i * 3);
            [[NSColor colorWithCalibratedWhite:0.15 alpha:0.55] set]; NSRectFill(NSMakeRect(floor(cx - gw / 2), y, gw, 1));
            [[NSColor colorWithCalibratedWhite:1 alpha:0.6] set];     NSRectFill(NSMakeRect(floor(cx - gw / 2), y + 1, gw, 1));
        } else {
            CGFloat x = floor(cx + i * 3);
            [[NSColor colorWithCalibratedWhite:0.15 alpha:0.55] set]; NSRectFill(NSMakeRect(x, floor(cy - gh / 2), 1, gh));
            [[NSColor colorWithCalibratedWhite:1 alpha:0.6] set];     NSRectFill(NSMakeRect(x + 1, floor(cy - gh / 2), 1, gh));
        }
    }
}

@end
