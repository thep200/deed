// OS9 Platinum title bar — pixel-accurate drawing per docs/PROMPT_os9_titlebar_objcpp.md.
// 5 states: Active (default) + Close/Zoom/Collapse pressed + Inactive.
// Active: close pinned left; right cluster Zoom (left) + Collapse (far right);
//         pinstripe breaks around the centered bold title.
// Inactive: flat #D6D6D6 background, NO pinstripe, NO buttons, with folder icon + gray text.
#import "widgets/OS9TitleBar.h"
#import "theme/OS9Theme.h"
#import "icons/OS9Glyphs.h"

// Sizes & insets. Visible bevel box = kBox (square), hit-area expanded around the box.
static const CGFloat kBox      = 16;  // visible bevel box (larger for a clear glyph)
static const CGFloat kHitPad   = 2;   // expand each edge -> hit-area ~20×20
static const CGFloat kEdge     = 5;   // reference inset (iconSideInset = kEdge+1) for panes/buttons
// LEFT inset close = left edge of Open Folder button (= iconSideInset). RIGHT inset collapse = right edge of Pretty.
static const CGFloat kInsetL   = kEdge + 1;       // close.left = 6 (aligned with Open Folder)
static const CGFloat kInsetR   = kEdge + 1 + 2;   // collapse.right from right edge = 8 (aligned with Pretty)
static const CGFloat kBtnGap   = 6;   // gap between zoom and collapse
static const CGFloat kGripGap  = 6;   // gap between button and pinstripe band
static const CGFloat kTitlePad = 8;   // plain gap breaking the pinstripe on each side of the title
static const CGFloat kFolder   = 16;  // folder icon (Inactive state)
static const CGFloat kFolderGap = 5;  // gap folder icon ↔ title

// Pressed button. 0=none, 1=close, 2=zoom, 3=collapse.
typedef NS_ENUM(int, OS9TBButton) { OS9TBNone = 0, OS9TBClose, OS9TBZoom, OS9TBHide };

@implementation OS9TitleBar {
    OS9TBButton _pressed;
}

- (BOOL)isFlipped { return NO; }

// Left edge of close = iconSideInset (= left edge of Open Folder button under the pane).
+ (CGFloat)iconSideInset { return kInsetL; }

- (BOOL)isActive { return self.window ? self.window.isKeyWindow : YES; }

// Visible bevel box (kBox square) vertically centered in bounds.
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

// Title width (measured with bold font) — to compute the pinstripe break / icon group alignment.
- (NSDictionary *)titleAttrsActive:(BOOL)active {
    CGFloat w = active ? 0.149 : 0.541;   // #262626 active / #8A8A8A inactive
    return @{NSFontAttributeName : [OS9Theme boldUiFont],
             NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:w alpha:1.0]};
}

- (void)drawRect:(NSRect)dirty {
    BOOL active = [self isActive];
    if (!active) { [self drawInactive]; return; }

    // Active bar background (#CCCCCC + bottom line #262626).
    [OS9Theme drawTitleBarFrameInRect:self.bounds];

    // Total pinstripe span (between the 2 button clusters) + plain gap around the title.
    CGFloat sx = NSMaxX([self closeRect]) + kGripGap;
    CGFloat sr = NSMinX([self zoomRect]) - kGripGap;
    NSRect tr = NSZeroRect;
    if (_title.length) {
        NSDictionary *attrs = [self titleAttrsActive:YES];
        NSSize sz = [_title sizeWithAttributes:attrs];
        CGFloat maxW = MAX(0, sr - sx - 2 * kTitlePad - 8);  // don't overlap the 2 bands/buttons
        CGFloat tw = MIN(sz.width, maxW);
        tr = NSMakeRect(floor(NSMidX(self.bounds) - tw / 2),
                        floor((self.bounds.size.height - sz.height) / 2), tw, sz.height);
    }

    // Pinstripe band: split into 2 regions (left/right) around the title; empty -> 1 continuous band.
    if (_title.length && tr.size.width > 0) {
        CGFloat gapL = NSMinX(tr) - kTitlePad, gapR = NSMaxX(tr) + kTitlePad;
        [self drawGripFrom:sx to:gapL];
        [self drawGripFrom:gapR to:sr];
    } else {
        [self drawGripFrom:sx to:sr];
    }

    // Bold #262626 title centered (on plain #CCCCCC background).
    if (tr.size.width > 0)
        [_title drawInRect:tr withAttributes:[self titleAttrsActive:YES]];

    // 3 buttons + pressed state.
    [OS9Theme drawTitleButtonInRect:[self closeRect] glyph:0 pressed:(_pressed == OS9TBClose)];
    [OS9Theme drawTitleButtonInRect:[self zoomRect]  glyph:1 pressed:(_pressed == OS9TBZoom)];
    [OS9Theme drawTitleButtonInRect:[self hideRect]  glyph:2 pressed:(_pressed == OS9TBHide)];
}

- (void)drawGripFrom:(CGFloat)x0 to:(CGFloat)x1 {
    if (x1 - x0 <= 0) return;
    [OS9Theme drawTitleGripInRect:NSMakeRect(x0, self.bounds.origin.y, x1 - x0, self.bounds.size.height)];
}

// Inactive: flat background + folder icon (color) + gray title, centered as a group. No buttons.
- (void)drawInactive {
    [OS9Theme drawTitleBarInactiveInRect:self.bounds];

    NSDictionary *attrs = [self titleAttrsActive:NO];
    NSSize sz = _title.length ? [_title sizeWithAttributes:attrs] : NSZeroSize;
    CGFloat tw = sz.width;
    // Clamp title width within the frame (leave room for inset + icon).
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
    // Inactive: no buttons -> click only activates + drags the window.
    if (![self isActive]) { [self.window performWindowDragWithEvent:e]; return; }

    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    OS9TBButton btn = [self hitTestButton:p];
    if (btn == OS9TBNone) { [self.window performWindowDragWithEvent:e]; return; }

    // Track until release: pressed only while the cursor stays in the hit-area (drag out -> cancel).
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
