#import "widgets/OS9Dropdown.h"
#import "theme/OS9Theme.h"

#pragma mark - OS9 custom dropdown (góc vuông, không dùng NSMenu hệ thống)

@interface OS9DropdownOverlay : NSView {
    NSArray<NSString *> *_items;
    NSInteger _selected, _hover;
    NSRect _listRect;
    CGFloat _rowH;
    void (^_onPick)(NSInteger);
    NSResponder *_prevResponder;
    NSTrackingArea *_ta;
}
@end

@implementation OS9DropdownOverlay

- (BOOL)isFlipped { return YES; }            // khớp toạ độ contentView (top-down)
- (BOOL)acceptsFirstResponder { return YES; }

- (void)layoutForItems:(NSArray<NSString *> *)items selected:(NSInteger)sel
                anchor:(NSView *)anchor onPick:(void (^)(NSInteger))onPick {
    _items = [items copy];
    _selected = sel;
    _hover = -1;
    _onPick = [onPick copy];
    _rowH = 22;
    _prevResponder = anchor.window.firstResponder;   // trả focus khi đóng

    NSView *content = self.superview;
    NSRect a = [anchor convertRect:anchor.bounds toView:content];
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont]};
    CGFloat w = a.size.width;
    for (NSString *t in items) w = MAX(w, [t sizeWithAttributes:attrs].width + 34);
    CGFloat h = items.count * _rowH + 2;

    CGFloat downY = NSMaxY(a) + 1;           // ngay dưới anchor
    CGFloat upY   = a.origin.y - h - 1;       // ngay trên anchor
    CGFloat y = (downY + h <= content.bounds.size.height || upY < 0) ? downY : upY;
    CGFloat x = a.origin.x;
    if (x + w > content.bounds.size.width) x = content.bounds.size.width - w - 2;
    if (x < 2) x = 2;
    _listRect = NSMakeRect(floor(x), floor(y), floor(w), floor(h));
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
    NSInteger i = (NSInteger)((p.y - (_listRect.origin.y + 1)) / _rowH);
    return (i >= 0 && i < (NSInteger)_items.count) ? i : -1;
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
    // Hộp danh sách: nền platinum + viền nét đen, GÓC VUÔNG.
    [[OS9Theme buttonFace] set];
    NSRectFill(_listRect);
    NSDictionary *norm = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : [NSColor blackColor]};
    NSDictionary *hi   = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : [NSColor whiteColor]};
    for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
        NSRect row = NSMakeRect(_listRect.origin.x, _listRect.origin.y + 1 + i * _rowH, _listRect.size.width, _rowH);
        BOOL hot = (i == _hover);
        if (hot) { [[NSColor colorWithCalibratedRed:0.2 green:0.2 blue:0.6 alpha:1.0] set]; NSRectFill(row); }
        NSDictionary *attrs = hot ? hi : norm;
        if (i == _selected)
            [@"✓" drawAtPoint:NSMakePoint(row.origin.x + 7, row.origin.y + (_rowH - 12) / 2) withAttributes:attrs];
        NSSize sz = [_items[i] sizeWithAttributes:attrs];
        [_items[i] drawAtPoint:NSMakePoint(row.origin.x + 22, row.origin.y + (_rowH - sz.height) / 2) withAttributes:attrs];
    }
    [[NSColor colorWithCalibratedWhite:0.15 alpha:1] set];
    NSBezierPath *border = [NSBezierPath bezierPathWithRect:NSInsetRect(_listRect, 0.5, 0.5)];
    border.lineWidth = 1.0;
    [border stroke];
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
