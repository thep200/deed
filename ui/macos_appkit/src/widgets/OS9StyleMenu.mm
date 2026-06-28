#import "widgets/OS9StyleMenu.h"
#import "theme/OS9Theme.h"

#pragma mark - OS9MenuEntry

@implementation OS9MenuEntry
+ (instancetype)entry:(NSString *)title action:(void (^)(void))action {
    OS9MenuEntry *e = [OS9MenuEntry new];
    e.title = title;
    e.action = action;
    return e;
}
+ (instancetype)separator {
    OS9MenuEntry *e = [OS9MenuEntry new];
    e.separator = YES;
    return e;
}
@end

#pragma mark - OS9ContextOverlay (self-drawn retro menu box — mirrors OS9DropdownOverlay)

static const CGFloat kRowH = 22;   // normal item row
static const CGFloat kSepH = 8;    // separator row
static const CGFloat kPadL = 22;   // left gutter (checkmark)
static const CGFloat kPadR = 14;   // right padding

@interface OS9ContextOverlay : NSView {
    NSArray<OS9MenuEntry *> *_entries;
    NSMutableArray<NSValue *> *_rowRects;   // per-entry rect (overlay coords), aligned with _entries
    NSRect _menuRect;
    NSInteger _hover;
    NSResponder *_prevResponder;
    NSTrackingArea *_ta;
}
@end

@implementation OS9ContextOverlay

- (BOOL)isFlipped { return YES; }            // top-down coords (match contentView)
- (BOOL)acceptsFirstResponder { return YES; }

- (void)layoutEntries:(NSArray<OS9MenuEntry *> *)entries anchor:(NSView *)anchor at:(NSPoint)windowPoint {
    _entries = [entries copy];
    _hover = -1;
    _prevResponder = anchor.window.firstResponder;

    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont]};
    CGFloat w = 120, h = 0;
    for (OS9MenuEntry *e in entries) {
        if (e.separator) { h += kSepH; continue; }
        w = MAX(w, [(e.title ?: @"") sizeWithAttributes:attrs].width + kPadL + kPadR);
        h += kRowH;
    }
    w = floor(MIN(w, 360));
    h = floor(h + 2);                         // +2 for the 1px frame top/bottom

    NSPoint p = [self convertPoint:windowPoint fromView:nil];   // window -> overlay (flipped) coords
    CGFloat bw = self.bounds.size.width, bh = self.bounds.size.height;
    CGFloat x = p.x, y = p.y;
    if (x + w > bw) x = bw - w;               // flip left near the right edge
    if (y + h > bh) y = p.y - h;              // flip up near the bottom edge
    if (x < 2) x = 2;
    if (y < 2) y = 2;
    _menuRect = NSMakeRect(floor(x), floor(y), w, h);

    // Pre-compute each entry's rect (for hit-testing + drawing).
    _rowRects = [NSMutableArray arrayWithCapacity:entries.count];
    CGFloat ry = _menuRect.origin.y + 1;
    for (OS9MenuEntry *e in entries) {
        CGFloat rh = e.separator ? kSepH : kRowH;
        [_rowRects addObject:[NSValue valueWithRect:NSMakeRect(_menuRect.origin.x, ry, w, rh)]];
        ry += rh;
    }
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_ta) [self removeTrackingArea:_ta];
    _ta = [[NSTrackingArea alloc] initWithRect:self.bounds
                                       options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways)
                                         owner:self userInfo:nil];
    [self addTrackingArea:_ta];
}

// Index of the selectable entry under p (-1 for separators / outside the box).
- (NSInteger)rowAt:(NSPoint)p {
    if (!NSPointInRect(p, _menuRect)) return -1;
    for (NSInteger i = 0; i < (NSInteger)_entries.count; i++) {
        if (_entries[i].separator) continue;
        if (NSPointInRect(p, _rowRects[i].rectValue)) return i;
    }
    return -1;
}

- (void)mouseMoved:(NSEvent *)e {
    NSInteger r = [self rowAt:[self convertPoint:e.locationInWindow fromView:nil]];
    if (r != _hover) { _hover = r; [self setNeedsDisplay:YES]; }
}
- (void)mouseExited:(NSEvent *)e { if (_hover != -1) { _hover = -1; [self setNeedsDisplay:YES]; } }

- (void)mouseDown:(NSEvent *)e {
    NSInteger r = [self rowAt:[self convertPoint:e.locationInWindow fromView:nil]];
    void (^action)(void) = nil;
    if (r >= 0) action = _entries[r].action;
    [self dismiss];
    if (action) action();
}
- (void)rightMouseDown:(NSEvent *)e { [self dismiss]; }   // second right-click closes it

- (void)keyDown:(NSEvent *)e {
    if (e.keyCode == 53) [self dismiss];     // Esc
    else [super keyDown:e];
}

- (void)dismiss {
    if (_prevResponder) [self.window makeFirstResponder:_prevResponder];
    [self removeFromSuperview];
}

- (void)drawRect:(NSRect)dirty {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    // Platinum box, square corners (no rounding).
    [[OS9Theme buttonFace] set];
    NSRectFill(_menuRect);
    [NSGraphicsContext restoreGraphicsState];

    for (NSInteger i = 0; i < (NSInteger)_entries.count; i++) {
        OS9MenuEntry *e = _entries[i];
        NSRect row = _rowRects[i].rectValue;
        if (e.separator) {
            [NSGraphicsContext saveGraphicsState];
            [[NSGraphicsContext currentContext] setShouldAntialias:NO];
            CGFloat y = floor(row.origin.y + row.size.height / 2.0);
            [[NSColor colorWithCalibratedWhite:0.55 alpha:1.0] set];
            NSRectFill(NSMakeRect(row.origin.x + 5, y,     row.size.width - 10, 1));
            [[NSColor colorWithCalibratedWhite:1.0  alpha:1.0] set];
            NSRectFill(NSMakeRect(row.origin.x + 5, y + 1, row.size.width - 10, 1));
            [NSGraphicsContext restoreGraphicsState];
            continue;
        }
        BOOL hot = (i == _hover);
        if (hot) {
            [NSGraphicsContext saveGraphicsState];
            [[NSGraphicsContext currentContext] setShouldAntialias:NO];
            [[NSColor colorWithCalibratedRed:0.2 green:0.2 blue:0.6 alpha:1.0] set];   // navy highlight
            NSRectFill(row);
            [NSGraphicsContext restoreGraphicsState];
        }
        NSColor *fg = hot ? [NSColor whiteColor] : [NSColor blackColor];
        NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : fg};
        if (e.checked)
            [OS9Theme drawCheckInRect:NSMakeRect(row.origin.x + 6, row.origin.y + (kRowH - 11) / 2, 11, 11)
                                color:fg];
        NSString *title = e.title ?: @"";
        NSSize sz = [title sizeWithAttributes:attrs];
        [title drawAtPoint:NSMakePoint(row.origin.x + kPadL, row.origin.y + floor((kRowH - sz.height) / 2))
            withAttributes:attrs];
    }

    // Crisp black box frame, square corners (NSFrameRect: no AA edge buildup on hover redraws).
    [[NSColor colorWithCalibratedWhite:0.15 alpha:1] set];
    NSFrameRect(_menuRect);
}

@end

void OS9ShowContextMenu(NSArray<OS9MenuEntry *> *entries, NSView *anchor, NSPoint windowPoint) {
    if (!entries.count || !anchor.window) return;
    NSView *content = anchor.window.contentView;
    OS9ContextOverlay *ov = [[OS9ContextOverlay alloc] initWithFrame:content.bounds];
    ov.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:ov positioned:NSWindowAbove relativeTo:nil];
    [ov layoutEntries:entries anchor:anchor at:windowPoint];
    [anchor.window makeFirstResponder:ov];
    [ov setNeedsDisplay:YES];
}
