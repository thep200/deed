// Title bar OS9 Platinum — vẽ pixel-accurate theo docs/PROMPT_os9_titlebar_objcpp.md.
// 5 trạng thái: Active (default) + Close/Zoom/Collapse pressed + Inactive.
// Active: close ghim trái; cụm phải Zoom (trái) + Collapse (ngoài cùng phải);
//         pinstripe ngắt quanh tiêu đề đậm căn giữa.
// Inactive: nền phẳng #D6D6D6, KHÔNG pinstripe, KHÔNG nút, có icon folder + chữ xám.
#import "widgets/OS9TitleBar.h"
#import "theme/OS9Theme.h"
#import "icons/OS9Glyphs.h"

// Kích thước & lề. Hộp bevel hiển thị = kBox (vuông), hit-area nới rộng quanh hộp.
static const CGFloat kBox      = 16;  // hộp bevel hiển thị (to hơn để glyph rõ)
static const CGFloat kHitPad   = 2;   // nới mỗi cạnh -> hit-area ~20×20
static const CGFloat kEdge     = 5;   // lề tham chiếu (iconSideInset = kEdge+1) cho pane/nút
// Lề TRÁI close = mép trái nút Open Folder (= iconSideInset). Lề PHẢI collapse = mép phải Pretty.
static const CGFloat kInsetL   = kEdge + 1;       // close.left = 6 (thẳng hàng Open Folder)
static const CGFloat kInsetR   = kEdge + 1 + 2;   // collapse.right từ mép phải = 8 (thẳng hàng Pretty)
static const CGFloat kBtnGap   = 6;   // gap giữa zoom và collapse
static const CGFloat kGripGap  = 6;   // gap giữa nút và dải pinstripe
static const CGFloat kTitlePad = 8;   // khoảng trơn ngắt pinstripe mỗi bên tiêu đề
static const CGFloat kFolder   = 16;  // icon folder (trạng thái Inactive)
static const CGFloat kFolderGap = 5;  // gap icon folder ↔ tiêu đề

// Nút đang nhấn (pressed). 0=không, 1=close, 2=zoom, 3=collapse.
typedef NS_ENUM(int, OS9TBButton) { OS9TBNone = 0, OS9TBClose, OS9TBZoom, OS9TBHide };

@implementation OS9TitleBar {
    OS9TBButton _pressed;
}

- (BOOL)isFlipped { return NO; }

// Mép trái close = iconSideInset (= mép trái nút Open Folder dưới pane).
+ (CGFloat)iconSideInset { return kInsetL; }

- (BOOL)isActive { return self.window ? self.window.isKeyWindow : YES; }

// Hộp bevel hiển thị (vuông kBox) căn giữa dọc trong bounds.
- (NSRect)closeRect {
    CGFloat y = floor((self.bounds.size.height - kBox) / 2);
    return NSMakeRect(kInsetL, y, kBox, kBox);
}
- (NSRect)hideRect {
    CGFloat y = floor((self.bounds.size.height - kBox) / 2);
    return NSMakeRect(self.bounds.size.width - kInsetR - kBox, y, kBox, kBox);
}
- (NSRect)zoomRect {
    NSRect h = [self hideRect];
    return NSMakeRect(h.origin.x - kBtnGap - kBox, h.origin.y, kBox, kBox);
}

// Bề rộng tiêu đề (đo theo font đậm) — để tính khoảng ngắt pinstripe / canh nhóm icon.
- (NSDictionary *)titleAttrsActive:(BOOL)active {
    CGFloat w = active ? 0.149 : 0.541;   // #262626 active / #8A8A8A inactive
    return @{NSFontAttributeName : [OS9Theme boldUiFont],
             NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:w alpha:1.0]};
}

- (void)drawRect:(NSRect)dirty {
    BOOL active = [self isActive];
    if (!active) { [self drawInactive]; return; }

    // Nền thanh active (#CCCCCC + line đáy #262626).
    [OS9Theme drawTitleBarFrameInRect:self.bounds];

    // Khoảng pinstripe tổng (giữa 2 cụm nút) + khoảng trơn quanh tiêu đề.
    CGFloat sx = NSMaxX([self closeRect]) + kGripGap;
    CGFloat sr = NSMinX([self zoomRect]) - kGripGap;
    NSRect tr = NSZeroRect;
    if (_title.length) {
        NSDictionary *attrs = [self titleAttrsActive:YES];
        NSSize sz = [_title sizeWithAttributes:attrs];
        CGFloat maxW = MAX(0, sr - sx - 2 * kTitlePad - 8);  // không đè lên 2 dải/nút
        CGFloat tw = MIN(sz.width, maxW);
        tr = NSMakeRect(floor(NSMidX(self.bounds) - tw / 2),
                        floor((self.bounds.size.height - sz.height) / 2), tw, sz.height);
    }

    // Dải pinstripe: ngắt thành 2 vùng (trái/phải) quanh tiêu đề; rỗng -> 1 dải liền.
    if (_title.length && tr.size.width > 0) {
        CGFloat gapL = NSMinX(tr) - kTitlePad, gapR = NSMaxX(tr) + kTitlePad;
        [self drawGripFrom:sx to:gapL];
        [self drawGripFrom:gapR to:sr];
    } else {
        [self drawGripFrom:sx to:sr];
    }

    // Tiêu đề đậm #262626 căn giữa (trên nền trơn #CCCCCC).
    if (tr.size.width > 0)
        [_title drawInRect:tr withAttributes:[self titleAttrsActive:YES]];

    // 3 nút + trạng thái pressed.
    [OS9Theme drawTitleButtonInRect:[self closeRect] glyph:0 pressed:(_pressed == OS9TBClose)];
    [OS9Theme drawTitleButtonInRect:[self zoomRect]  glyph:1 pressed:(_pressed == OS9TBZoom)];
    [OS9Theme drawTitleButtonInRect:[self hideRect]  glyph:2 pressed:(_pressed == OS9TBHide)];
}

- (void)drawGripFrom:(CGFloat)x0 to:(CGFloat)x1 {
    if (x1 - x0 <= 0) return;
    [OS9Theme drawTitleGripInRect:NSMakeRect(x0, self.bounds.origin.y, x1 - x0, self.bounds.size.height)];
}

// Inactive: nền phẳng + icon folder (màu) + tiêu đề xám, căn giữa thành nhóm. Không nút.
- (void)drawInactive {
    [OS9Theme drawTitleBarInactiveInRect:self.bounds];

    NSDictionary *attrs = [self titleAttrsActive:NO];
    NSSize sz = _title.length ? [_title sizeWithAttributes:attrs] : NSZeroSize;
    CGFloat tw = sz.width;
    // Kẹp bề rộng tiêu đề trong khung (chừa lề + icon).
    CGFloat avail = self.bounds.size.width - 2 * kInsetL - kFolder - kFolderGap;
    if (tw > avail) tw = MAX(0, avail);

    CGFloat group = kFolder + (tw > 0 ? kFolderGap + tw : 0);
    CGFloat startX = floor(NSMidX(self.bounds) - group / 2);
    CGFloat iconY = floor((self.bounds.size.height - kFolder) / 2);

    NSImage *folder = OS9FolderImage(kFolder);
    [folder drawInRect:NSMakeRect(startX, iconY, kFolder, kFolder)
              fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];

    if (tw > 0) {
        NSRect tr = NSMakeRect(startX + kFolder + kFolderGap,
                               floor((self.bounds.size.height - sz.height) / 2), tw, sz.height);
        [_title drawInRect:tr withAttributes:attrs];
    }
}

- (NSRect)hitRectFor:(NSRect)box { return NSInsetRect(box, -kHitPad, -kHitPad); }

- (OS9TBButton)hitTestButton:(NSPoint)p {
    if (NSPointInRect(p, [self hitRectFor:[self closeRect]])) return OS9TBClose;
    if (NSPointInRect(p, [self hitRectFor:[self zoomRect]]))  return OS9TBZoom;
    if (NSPointInRect(p, [self hitRectFor:[self hideRect]]))  return OS9TBHide;
    return OS9TBNone;
}

- (void)mouseDown:(NSEvent *)e {
    // Inactive: không có nút -> click chỉ kích hoạt + kéo cửa sổ.
    if (![self isActive]) { [self.window performWindowDragWithEvent:e]; return; }

    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    OS9TBButton btn = [self hitTestButton:p];
    if (btn == OS9TBNone) { [self.window performWindowDragWithEvent:e]; return; }

    // Theo dõi tới khi nhả: pressed chỉ khi con trỏ còn trong hit-area (kéo ra -> huỷ).
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
