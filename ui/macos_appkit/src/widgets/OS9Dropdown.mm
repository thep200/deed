#import "widgets/OS9Dropdown.h"
#import "theme/OS9Theme.h"

#pragma mark - OS9 custom dropdown (square corners, no system NSMenu)

@interface OS9DropdownOverlay : NSView {
    NSArray<NSString *> *_items;
    NSInteger _selected, _hover;
    NSRect _listRect;
    CGFloat _rowH;
    CGFloat _scrollOff;   // vertical scroll offset (pts) when the list is taller than the box
    CGFloat _maxScroll;   // = contentHeight - visibleHeight (0 when everything fits)
    void (^_onPick)(NSInteger);
    NSResponder *_prevResponder;
    NSTrackingArea *_ta;
}
@end

@implementation OS9DropdownOverlay

- (BOOL)isFlipped { return YES; }            // match contentView coords (top-down)
- (BOOL)acceptsFirstResponder { return YES; }

- (void)layoutForItems:(NSArray<NSString *> *)items selected:(NSInteger)sel
                anchor:(NSView *)anchor onPick:(void (^)(NSInteger))onPick {
    _items = [items copy];
    _selected = sel;
    _hover = -1;
    _onPick = [onPick copy];
    _rowH = 22;
    _prevResponder = anchor.window.firstResponder;   // restore focus on close

    NSView *content = self.superview;
    NSRect a = [anchor convertRect:anchor.bounds toView:content];
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont]};
    CGFloat w = a.size.width;
    for (NSString *t in items) w = MAX(w, [t sizeWithAttributes:attrs].width + 34);
    w = MIN(w, 360);                          // cap width -> long names get "…" truncated (see drawRect)

    // Cap height: never exceed the space above/below the anchor, and at most ~12 rows. Overflow scrolls.
    CGFloat fullH = items.count * _rowH + 2;
    CGFloat downSpace = content.bounds.size.height - (NSMaxY(a) + 1) - 2;   // room below the anchor
    CGFloat upSpace = a.origin.y - 1 - 2;                                    // room above the anchor
    CGFloat hardMax = _rowH * 12 + 2;                                        // hard cap ~12 visible rows
    BOOL down = (downSpace >= upSpace);
    CGFloat avail = MAX(down ? downSpace : upSpace, _rowH + 2);              // at least one row
    CGFloat h = MIN(fullH, MIN(hardMax, avail));
    CGFloat y = down ? (NSMaxY(a) + 1) : (a.origin.y - h - 1);
    CGFloat x = a.origin.x;
    if (x + w > content.bounds.size.width) x = content.bounds.size.width - w - 2;
    if (x < 2) x = 2;
    _listRect = NSMakeRect(floor(x), floor(y), floor(w), floor(h));

    // Scrolling: content vs visible. Start scrolled so the selected row is in view.
    CGFloat visibleH = _listRect.size.height - 2;
    CGFloat contentH = items.count * _rowH;
    _maxScroll = MAX(0, contentH - visibleH);
    _scrollOff = 0;
    if (sel >= 0 && _maxScroll > 0) {
        CGFloat selTop = sel * _rowH, selBot = selTop + _rowH;
        if (selBot > visibleH) _scrollOff = MIN(_maxScroll, selBot - visibleH);
    }
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_ta) [self removeTrackingArea:_ta];
    _ta = [[NSTrackingArea alloc] initWithRect:_listRect
                                       options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways)
                                         owner:self userInfo:nil];
    [self addTrackingArea:_ta];
}

- (NSInteger)rowAt:(NSPoint)p {
    if (!NSPointInRect(p, _listRect)) return -1;
    NSInteger i = (NSInteger)((p.y - (_listRect.origin.y + 1) + _scrollOff) / _rowH);
    return (i >= 0 && i < (NSInteger)_items.count) ? i : -1;
}

- (void)scrollWheel:(NSEvent *)e {
    if (_maxScroll <= 0) return;
    _scrollOff -= e.scrollingDeltaY;
    if (_scrollOff < 0) _scrollOff = 0;
    if (_scrollOff > _maxScroll) _scrollOff = _maxScroll;
    NSInteger r = [self rowAt:[self convertPoint:e.locationInWindow fromView:nil]];
    _hover = r;
    [self setNeedsDisplay:YES];
}

- (void)mouseMoved:(NSEvent *)e {
    NSInteger r = [self rowAt:[self convertPoint:e.locationInWindow fromView:nil]];
    if (r != _hover) { _hover = r; [self setNeedsDisplay:YES]; }
}
- (void)mouseExited:(NSEvent *)e { if (_hover != -1) { _hover = -1; [self setNeedsDisplay:YES]; } }

- (void)mouseDown:(NSEvent *)e {
    NSInteger r = [self rowAt:[self convertPoint:e.locationInWindow fromView:nil]];
    void (^pick)(NSInteger) = _onPick;
    [self dismiss];
    if (r >= 0 && pick) pick(r);
}

- (void)keyDown:(NSEvent *)e {
    if (e.keyCode == 53) [self dismiss];     // Esc
    else [super keyDown:e];
}

- (void)dismiss {
    if (_prevResponder) [self.window makeFirstResponder:_prevResponder];
    [self removeFromSuperview];
}

- (void)drawRect:(NSRect)dirty {
    // List box: platinum background + black outline, SQUARE CORNERS.
    [[OS9Theme buttonFace] set];
    NSRectFill(_listRect);
    NSDictionary *norm = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : [NSColor blackColor]};
    NSDictionary *hi   = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : [NSColor whiteColor]};
    // Truncating "…" variant built ONCE before the loop (shared constant paragraph style) — previously
    // alloc'd NSMutableParagraphStyle + mutableCopy per row.
    NSParagraphStyle *trunc = [OS9Theme truncatingTailStyle];
    NSDictionary *normTr = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : [NSColor blackColor], NSParagraphStyleAttributeName : trunc};
    NSDictionary *hiTr   = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : [NSColor whiteColor], NSParagraphStyleAttributeName : trunc};
    BOOL scrolls = (_maxScroll > 0);
    CGFloat textRight = NSMaxX(_listRect) - 6 - (scrolls ? 5 : 0);   // leave room for the scrollbar
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(_listRect);                                            // clip rows to the box while scrolling
    for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
        CGFloat rowY = _listRect.origin.y + 1 + i * _rowH - _scrollOff;
        if (rowY + _rowH < _listRect.origin.y || rowY > NSMaxY(_listRect)) continue;   // offscreen -> skip
        NSRect row = NSMakeRect(_listRect.origin.x, rowY, _listRect.size.width, _rowH);
        BOOL hot = (i == _hover);
        if (hot) { [[NSColor colorWithCalibratedRed:0.2 green:0.2 blue:0.6 alpha:1.0] set]; NSRectFill(row); }
        NSDictionary *attrs = hot ? hi : norm;
        if (i == _selected)
            [OS9Theme drawCheckInRect:NSMakeRect(row.origin.x + 6, row.origin.y + (_rowH - 11) / 2, 11, 11)
                                color:(hot ? [NSColor whiteColor] : [NSColor blackColor])];
        // Truncate "…" to row width.
        NSDictionary *trAttrs = hot ? hiTr : normTr;
        CGFloat textX = row.origin.x + 22;
        NSSize sz = [_items[i] sizeWithAttributes:attrs];
        NSRect textRect = NSMakeRect(textX, row.origin.y + (_rowH - sz.height) / 2, textRight - textX, sz.height);
        [_items[i] drawInRect:textRect withAttributes:trAttrs];
    }
    [NSGraphicsContext restoreGraphicsState];

    // Scrollbar thumb on the right edge when the list overflows.
    if (scrolls) {
        CGFloat visibleH = _listRect.size.height - 2;
        CGFloat contentH = _items.count * _rowH;
        CGFloat thumbH = MAX(18, visibleH * visibleH / contentH);
        CGFloat thumbY = _listRect.origin.y + 1 + (_scrollOff / _maxScroll) * (visibleH - thumbH);
        [[NSColor colorWithCalibratedWhite:0.5 alpha:1] set];
        NSRectFill(NSMakeRect(NSMaxX(_listRect) - 4, floor(thumbY), 3, floor(thumbH)));
    }

    // Box border: NSFrameRect (no AA, crisp, drawn within _listRect) — redrawing on hover doesn't
    // accumulate edge alpha like NSBezierPath stroke antialias would (view not layer-backed).
    [[NSColor colorWithCalibratedWhite:0.15 alpha:1] set];
    NSFrameRect(_listRect);
}

@end

void OS9ShowDropdown(NSArray<NSString *> *items, NSInteger selected, NSView *anchor,
                     void (^onPick)(NSInteger)) {
    if (!items.count || !anchor.window) return;
    NSView *content = anchor.window.contentView;
    OS9DropdownOverlay *ov = [[OS9DropdownOverlay alloc] initWithFrame:content.bounds];
    ov.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:ov positioned:NSWindowAbove relativeTo:nil];
    [ov layoutForItems:items selected:selected anchor:anchor onPick:onPick];
    [anchor.window makeFirstResponder:ov];
    [ov setNeedsDisplay:YES];
}
