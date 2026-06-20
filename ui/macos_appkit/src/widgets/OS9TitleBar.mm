// Title bar OS9 Platinum vẽ lại theo SPEC_title_bar_redraw.md.
// Bỏ icon ứng dụng + title text: vùng giữa là một dải vân grip liền mạch.
// Close ghim trái; cụm phải Zoom (trái) + Hide (phải, ngoài cùng) — theo asset.
#import "widgets/OS9TitleBar.h"
#import "theme/OS9Theme.h"

// Kích thước & lề (theo §5 của spec).
static const CGFloat kBtn      = 14;  // canvas nút: to hơn gốc 13 nhưng nhỏ lại để lề trên/dưới mượt trong title 21px
static const CGFloat kEdge     = 5;   // lề tham chiếu (iconSideInset = kEdge+1) cho pane/nút
// Lề TRÁI close = mép trái nút Open Folder (= iconSideInset). Lề PHẢI hide = mép phải nút Pretty.
static const CGFloat kInsetL   = kEdge + 1;       // close.left = 6 (thẳng hàng Open Folder)
static const CGFloat kInsetR   = kEdge + 1 + 2;   // hide.right từ mép phải = 8 (thẳng hàng Pretty: pane -2px)
static const CGFloat kBtnGap   = 6;   // gap giữa zoom và hide
static const CGFloat kGripGap  = 6;   // gap giữa nút và dải vân grip

// Nút đang nhấn (Active). 0=không, 1=close, 2=zoom, 3=hide.
typedef NS_ENUM(int, OS9TBButton) { OS9TBNone = 0, OS9TBClose, OS9TBZoom, OS9TBHide };

@implementation OS9TitleBar {
    OS9TBButton _pressed;
}

- (BOOL)isFlipped { return NO; }

// Mép trái close = iconSideInset (= mép trái nút Open Folder dưới pane).
+ (CGFloat)iconSideInset { return kInsetL; }

// Canvas nút căn giữa dọc trong bounds.
- (NSRect)closeRect {
    CGFloat y = floor((self.bounds.size.height - kBtn) / 2);
    return NSMakeRect(kInsetL, y, kBtn, kBtn);                 // thẳng hàng mép trái Open Folder
}
- (NSRect)hideRect {
    CGFloat y = floor((self.bounds.size.height - kBtn) / 2);
    return NSMakeRect(self.bounds.size.width - kInsetR - kBtn, y, kBtn, kBtn);  // mép phải thẳng hàng Pretty
}
- (NSRect)zoomRect {
    NSRect h = [self hideRect];
    return NSMakeRect(h.origin.x - kBtnGap - kBtn, h.origin.y, kBtn, kBtn);
}

- (void)drawRect:(NSRect)dirty {
    // Khung thanh: viền + nền + inner-shadow.
    [OS9Theme drawTitleBarFrameInRect:self.bounds];

    // Dải vân grip liền mạch giữa close.right+gap và zoom.left−gap.
    CGFloat sx = NSMaxX([self closeRect]) + kGripGap;
    CGFloat sr = NSMinX([self zoomRect]) - kGripGap;
    NSRect grip = NSMakeRect(sx, self.bounds.origin.y, sr - sx, self.bounds.size.height);
    [OS9Theme drawTitleGripInRect:grip];

    // Tên request căn giữa (chỉ chữ, không icon) — nền nhỏ tách khỏi vân grip cho dễ đọc.
    if (_title.length) {
        NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme boldUiFont],
                                NSForegroundColorAttributeName : [NSColor blackColor]};
        NSSize sz = [_title sizeWithAttributes:attrs];
        // Kẹp bề rộng trong khoảng grip (không đè lên nút).
        CGFloat maxW = MAX(0, (NSMaxX(grip) - 8) - (NSMinX(grip) + 8));
        CGFloat tw = MIN(sz.width, maxW);
        if (tw > 0) {
            NSRect tr = NSMakeRect(floor(NSMidX(self.bounds) - tw / 2),
                                   floor((self.bounds.size.height - sz.height) / 2), tw, sz.height);
            [[OS9Theme face] set];
            NSRectFill(NSInsetRect(tr, -6, -1));   // nền #CCCCCC phủ vân phía sau chữ
            [_title drawInRect:tr withAttributes:attrs];
        }
    }

    // 3 nút (close trống / zoom ô / hide thanh) + state Active khi đang nhấn.
    [OS9Theme drawTitleButtonInRect:[self closeRect] glyph:0 active:(_pressed == OS9TBClose)];
    [OS9Theme drawTitleButtonInRect:[self zoomRect]  glyph:1 active:(_pressed == OS9TBZoom)];
    [OS9Theme drawTitleButtonInRect:[self hideRect]  glyph:2 active:(_pressed == OS9TBHide)];
}

- (OS9TBButton)hitTestButton:(NSPoint)p {
    if (NSPointInRect(p, [self closeRect])) return OS9TBClose;
    if (NSPointInRect(p, [self zoomRect]))  return OS9TBZoom;
    if (NSPointInRect(p, [self hideRect]))  return OS9TBHide;
    return OS9TBNone;
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    OS9TBButton btn = [self hitTestButton:p];
    if (btn == OS9TBNone) { [self.window performWindowDragWithEvent:e]; return; }

    // Theo dõi tới khi nhả: Active chỉ khi con trỏ còn trong bounds nút (kéo ra -> huỷ).
    _pressed = btn;
    [self setNeedsDisplay:YES];
    while (YES) {
        NSEvent *ev = [self.window nextEventMatchingMask:
                       (NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged)];
        NSPoint q = [self convertPoint:ev.locationInWindow fromView:nil];
        OS9TBButton want = ([self hitTestButton:q] == btn) ? btn : OS9TBNone;
        if (want != _pressed) { _pressed = want; [self setNeedsDisplay:YES]; }
        if (ev.type == NSEventTypeLeftMouseUp) break;
    }

    BOOL fire = (_pressed == btn);
    _pressed = OS9TBNone;
    [self setNeedsDisplay:YES];
    if (!fire) return;

    switch (btn) {
        case OS9TBClose:
            if (_closeTarget && _closeAction) [NSApp sendAction:_closeAction to:_closeTarget from:self];
            else [self.window performClose:nil];
            break;
        case OS9TBZoom:
            if (_zoomTarget && _zoomAction) [NSApp sendAction:_zoomAction to:_zoomTarget from:self];
            else [self.window performZoom:nil];
            break;
        case OS9TBHide:
            if (_collapseTarget && _collapseAction) [NSApp sendAction:_collapseAction to:_collapseTarget from:self];
            else [self.window miniaturize:nil];
            break;
        default: break;
    }
}

@end
