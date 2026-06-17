#import "OS9Widgets.h"
#import "OS9Theme.h"

#pragma mark - OS9BevelButton

@implementation OS9BevelButton {
    BOOL _pressed;
}

- (instancetype)initWithTitle:(NSString *)title target:(id)target action:(SEL)action {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 80, 22)])) {
        _title = [title copy];
        _enabledState = YES;
        self.target = target;
        self.action = action;
        self.focusRingType = NSFocusRingTypeNone; // không vẽ focus ring (tránh "viền đậm" dính)
    }
    return self;
}

- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstResponder { return NO; } // không nhận focus -> không có viền focus

- (void)drawRect:(NSRect)dirty {
    [OS9Theme drawButtonInRect:self.bounds pressed:_pressed isDefault:_isDefault];
    if (_icon) {
        NSSize is = _icon.size;
        NSRect ir = NSMakeRect(floor((self.bounds.size.width - is.width) / 2),
                               floor((self.bounds.size.height - is.height) / 2) + (_pressed ? -1 : 0),
                               is.width, is.height);
        [_icon drawInRect:ir fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                 fraction:(_enabledState ? 1.0 : 0.5)];
        return;
    }
    NSColor *fg = [OS9Theme buttonFGPressed:_pressed enabled:_enabledState];
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont],
                            NSForegroundColorAttributeName : fg};
    NSString *title = _title ?: @"";
    NSSize sz = [title sizeWithAttributes:attrs];
    CGFloat cw = self.bounds.size.width - (_dropdown ? 16 : 0); // chừa chỗ mũi tên
    NSPoint pt = NSMakePoint(floor((cw - sz.width) / 2),
                             floor((self.bounds.size.height - sz.height) / 2) + (_pressed ? -1 : 0));
    [title drawAtPoint:pt withAttributes:attrs];
    if (_dropdown) [OS9Theme drawDropdownArrowInRect:self.bounds];
}

- (void)setDropdown:(BOOL)d { _dropdown = d; [self setNeedsDisplay:YES]; }

- (void)setIcon:(NSImage *)icon { _icon = icon; [self setNeedsDisplay:YES]; }

- (void)mouseDown:(NSEvent *)e {
    if (!_enabledState) return;
    _pressed = YES; [self setNeedsDisplay:YES];
}
- (void)mouseDragged:(NSEvent *)e {
    if (!_enabledState) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    BOOL in = NSPointInRect(p, self.bounds);
    if (in != _pressed) { _pressed = in; [self setNeedsDisplay:YES]; }
}
- (void)mouseUp:(NSEvent *)e {
    if (!_enabledState) return;
    BOOL fire = _pressed;
    _pressed = NO;
    [self setNeedsDisplay:YES];
    [self displayIfNeeded]; // vẽ lại trạng thái "nhả" NGAY, trước khi action mở modal/menu
    if (fire && self.action) [NSApp sendAction:self.action to:self.target from:self];
}

- (void)setTitle:(NSString *)title { _title = [title copy]; [self setNeedsDisplay:YES]; }
- (void)setEnabledState:(BOOL)e { _enabledState = e; [self setNeedsDisplay:YES]; }

@end

#pragma mark - OS9PopupButton

@implementation OS9PopupButton {
    BOOL _pressed;
}

- (instancetype)initWithItems:(NSArray<NSString *> *)items target:(id)target action:(SEL)action {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 90, 22)])) {
        _itemTitles = [items copy];
        _selectedIndex = 0;
        self.target = target;
        self.action = action;
        self.focusRingType = NSFocusRingTypeNone;
    }
    return self;
}

- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstResponder { return NO; }

- (NSString *)selectedTitle {
    if (_selectedIndex >= 0 && _selectedIndex < (NSInteger)_itemTitles.count) return _itemTitles[_selectedIndex];
    return @"";
}

- (void)selectTitle:(NSString *)title {
    NSInteger i = [_itemTitles indexOfObject:title];
    if (i != NSNotFound) { _selectedIndex = i; [self setNeedsDisplay:YES]; }
}

- (void)drawRect:(NSRect)dirty {
    [OS9Theme drawButtonInRect:self.bounds pressed:_pressed isDefault:NO];
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont],
                            NSForegroundColorAttributeName : [OS9Theme buttonFGPressed:_pressed enabled:YES]};
    NSString *t = self.selectedTitle;
    NSSize sz = [t sizeWithAttributes:attrs];
    [t drawAtPoint:NSMakePoint(7, floor((self.bounds.size.height - sz.height) / 2)) withAttributes:attrs];
    [OS9Theme drawDropdownArrowInRect:self.bounds]; // ▾ + vạch ngăn theo dropdown.svg
}

- (void)mouseDown:(NSEvent *)e {
    _pressed = YES; [self setNeedsDisplay:YES]; [self displayIfNeeded];
    __weak OS9PopupButton *ws = self;
    OS9ShowDropdown(_itemTitles, _selectedIndex, self, ^(NSInteger idx) {
        OS9PopupButton *s = ws; if (!s) return;
        s.selectedIndex = idx;
        [s setNeedsDisplay:YES];
        if (s.action) [NSApp sendAction:s.action to:s.target from:s];
    });
    _pressed = NO; [self setNeedsDisplay:YES];   // overlay là modeless -> nhả nút ngay
}

@end

#pragma mark - OS9TitleBar

@implementation OS9Window
// Borderless mặc định KHÔNG được làm key/main -> ép cho phép để gõ phím/active được.
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

#pragma mark - OS9TitleBar

@implementation OS9TitleBar

- (BOOL)isFlipped { return NO; }

- (void)drawRect:(NSRect)dirty {
    BOOL active = self.window.isKeyWindow;
    [OS9Theme drawStripedTitleInRect:self.bounds active:active];

    // 3 ô điều khiển kiểu Mac: close (trái), collapse + zoom (phải) — theo *_box.svg.
    [OS9Theme drawMacControlBox:[self closeRect] glyph:0];
    [OS9Theme drawMacControlBox:[self collapseRect] glyph:1];
    [OS9Theme drawMacControlBox:[self zoomRect] glyph:2];

    // tiêu đề căn giữa
    if (_title.length) {
        NSDictionary *attrs = @{NSFontAttributeName : [NSFont boldSystemFontOfSize:11],
                                NSForegroundColorAttributeName : [NSColor blackColor]};
        NSSize sz = [_title sizeWithAttributes:attrs];
        NSRect tr = NSMakeRect((self.bounds.size.width - sz.width) / 2,
                               (self.bounds.size.height - sz.height) / 2, sz.width, sz.height);
        [[OS9Theme face] set];
        NSRectFill(NSInsetRect(tr, -6, -1));
        [_title drawInRect:tr withAttributes:attrs];
    }
}

// close BÊN TRÁI; collapse (hide) + zoom (phóng to) BÊN PHẢI (như Mac OS 9).
// Icon to hơn (18px) cho dễ nhìn/bấm.
- (NSRect)closeRect    { CGFloat s = 18; return NSMakeRect(8, (self.bounds.size.height - s) / 2, s, s); }
- (NSRect)zoomRect     { CGFloat s = 18; return NSMakeRect(self.bounds.size.width - 8 - s, (self.bounds.size.height - s) / 2, s, s); }
- (NSRect)collapseRect { CGFloat s = 18; return NSMakeRect(self.bounds.size.width - 8 - s - 22, (self.bounds.size.height - s) / 2, s, s); }

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (NSPointInRect(p, [self closeRect])) {
        if (_closeTarget && _closeAction) [NSApp sendAction:_closeAction to:_closeTarget from:self];
        else [self.window performClose:nil];
        return;
    }
    if (NSPointInRect(p, [self collapseRect])) {
        if (_collapseTarget && _collapseAction) [NSApp sendAction:_collapseAction to:_collapseTarget from:self];
        else [self.window miniaturize:nil];
        return;
    }
    if (NSPointInRect(p, [self zoomRect])) {
        if (_zoomTarget && _zoomAction) [NSApp sendAction:_zoomAction to:_zoomTarget from:self];
        else [self.window performZoom:nil];
        return;
    }
    // còn lại: kéo cửa sổ
    [self.window performWindowDragWithEvent:e];
}

@end

#pragma mark - OS9BackgroundView

@implementation OS9BackgroundView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    [[OS9Theme face] set];
    NSRectFill(self.bounds);
}
@end

#pragma mark - OS9Divider

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

#pragma mark - OS9InsetView

@implementation OS9InsetView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    [[OS9Theme face] set];
    NSRectFill(self.bounds);
    [OS9Theme drawInsetInRect:self.bounds];
}
@end

NSImage *OS9GearImage(CGFloat size) {
    return [NSImage imageWithSize:NSMakeSize(size, size)
                          flipped:NO
                   drawingHandler:^BOOL(NSRect r) {
        CGFloat cx = size / 2, cy = size / 2;
        CGFloat rOut = size * 0.46;   // đỉnh răng
        CGFloat rRoot = size * 0.32;  // chân răng (vành)
        int teeth = 8;
        CGFloat pitch = 2 * M_PI / teeth;
        CGFloat tipHalf = pitch * 0.24;   // nửa bề rộng góc của đỉnh răng (răng vuông, rõ)

        // Răng vuông (flat-top) quanh vành -> bánh răng rõ ràng thay vì hình sao.
        NSBezierPath *gear = [NSBezierPath bezierPath];
        for (int i = 0; i < teeth; i++) {
            CGFloat a = i * pitch;
            NSPoint p1 = NSMakePoint(cx + rRoot * cos(a - tipHalf), cy + rRoot * sin(a - tipHalf));
            NSPoint p2 = NSMakePoint(cx + rOut  * cos(a - tipHalf), cy + rOut  * sin(a - tipHalf));
            NSPoint p3 = NSMakePoint(cx + rOut  * cos(a + tipHalf), cy + rOut  * sin(a + tipHalf));
            NSPoint p4 = NSMakePoint(cx + rRoot * cos(a + tipHalf), cy + rRoot * sin(a + tipHalf));
            if (i == 0) [gear moveToPoint:p1]; else [gear lineToPoint:p1];
            [gear lineToPoint:p2];
            [gear lineToPoint:p3];
            [gear lineToPoint:p4];
        }
        [gear closePath];

        // Kiểu "viền": vẽ nét bao (outline) đậm thay vì tô đặc.
        CGFloat lw = MAX(1.0, floor(size * 0.09));
        gear.lineWidth = lw;
        gear.lineJoinStyle = NSLineJoinStyleMiter;
        [[NSColor blackColor] set];
        [gear stroke];

        // lỗ trục ở giữa (vòng tròn viền)
        CGFloat hr = size * 0.15;
        NSBezierPath *hole = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - hr, cy - hr, hr * 2, hr * 2)];
        hole.lineWidth = lw;
        [hole stroke];
        return YES;
    }];
}

NSImage *OS9SendImage(CGFloat size) {
    return [NSImage imageWithSize:NSMakeSize(size, size) flipped:NO
                   drawingHandler:^BOOL(NSRect r) {
        CGFloat s = size;
        // Máy bay giấy hướng phải — đường VIỀN mảnh (hairline) + nếp gấp giữa.
        NSBezierPath *body = [NSBezierPath bezierPath];
        [body moveToPoint:NSMakePoint(s*0.90, s*0.50)];   // mũi (phải)
        [body lineToPoint:NSMakePoint(s*0.12, s*0.82)];   // góc trên-trái
        [body lineToPoint:NSMakePoint(s*0.40, s*0.50)];   // lõm giữa
        [body lineToPoint:NSMakePoint(s*0.12, s*0.18)];   // góc dưới-trái
        [body closePath];
        body.lineWidth = 0.6;                              // mảnh hơn (trước 1.0)
        body.lineJoinStyle = NSLineJoinStyleMiter;
        [[NSColor blackColor] set];
        [body stroke];
        return YES;
    }];
}

NSImage *OS9SpinnerImage(CGFloat size, CGFloat phase) {
    return [NSImage imageWithSize:NSMakeSize(size, size) flipped:NO
                   drawingHandler:^BOOL(NSRect r) {
        CGFloat cx = size/2, cy = size/2;
        CGFloat rIn = size*0.22, rOut = size*0.44;
        int spokes = 8;
        CGFloat lw = MAX(1.5, size*0.12);
        CGFloat base = phase * 2 * M_PI;          // pha quay
        for (int i = 0; i < spokes; i++) {
            CGFloat a = base + i * (2*M_PI/spokes);
            CGFloat alpha = 0.20 + 0.80 * ((CGFloat)i / (spokes-1)); // mờ dần -> hiệu ứng quay
            NSBezierPath *sp = [NSBezierPath bezierPath];
            sp.lineWidth = lw;
            sp.lineCapStyle = NSLineCapStyleRound;
            [sp moveToPoint:NSMakePoint(cx + rIn*cos(a),  cy + rIn*sin(a))];
            [sp lineToPoint:NSMakePoint(cx + rOut*cos(a), cy + rOut*sin(a))];
            [[NSColor colorWithCalibratedWhite:0.0 alpha:alpha] set];
            [sp stroke];
        }
        return YES;
    }];
}

#pragma mark - OS9Scroller (theo scrollbar.svg)

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

@implementation OS9SerratedInset
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    [[OS9Theme face] set];
    NSRectFill(self.bounds);
    NSBezierPath *p = [OS9Theme serratedPathInRect:NSInsetRect(self.bounds, 1, 1)];
    [[NSColor whiteColor] set];
    [p fill];
    [[OS9Theme frame] set];
    p.lineWidth = 1.0;
    [p stroke];
}
@end

#pragma mark - OS9MenuItemView (item dropdown kiểu OS9)

@interface OS9MenuItemView : NSView
@end

@implementation OS9MenuItemView {
    BOOL _hover;
    NSTrackingArea *_ta;
}
- (BOOL)isFlipped { return YES; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_ta) [self removeTrackingArea:_ta];
    _ta = [[NSTrackingArea alloc] initWithRect:self.bounds
                                       options:(NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect)
                                         owner:self userInfo:nil];
    [self addTrackingArea:_ta];
}
- (void)mouseEntered:(NSEvent *)e { _hover = YES; [self setNeedsDisplay:YES]; }
- (void)mouseExited:(NSEvent *)e { _hover = NO; [self setNeedsDisplay:YES]; }

- (void)drawRect:(NSRect)r {
    NSMenuItem *item = self.enclosingMenuItem;
    BOOL hl = _hover;
    // nền: chọn -> xanh tím #333399 ; thường -> platinum
    [(hl ? [NSColor colorWithCalibratedRed:0.2 green:0.2 blue:0.6 alpha:1.0] : [OS9Theme buttonFace]) set];
    NSRectFill(self.bounds);
    // tick nếu item đang chọn (state On)
    NSColor *fg = hl ? [NSColor whiteColor] : [NSColor blackColor];
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : fg};
    if (item.state == NSControlStateValueOn) {
        [@"✓" drawAtPoint:NSMakePoint(6, floor((self.bounds.size.height - 12) / 2)) withAttributes:attrs];
    }
    NSSize sz = [(item.title ?: @"") sizeWithAttributes:attrs];
    [(item.title ?: @"") drawAtPoint:NSMakePoint(20, floor((self.bounds.size.height - sz.height) / 2)) withAttributes:attrs];
}

- (void)mouseUp:(NSEvent *)e {
    NSMenuItem *item = self.enclosingMenuItem;
    [item.menu cancelTracking];
    if (item.action) [NSApp sendAction:item.action to:item.target from:item];
}
@end

void OS9StyleMenu(NSMenu *menu) {
    for (NSMenuItem *it in menu.itemArray) {
        if (it.isSeparatorItem || it.submenu || it.view) continue;
        NSSize sz = [(it.title ?: @"") sizeWithAttributes:@{NSFontAttributeName : [OS9Theme uiFont]}];
        CGFloat w = MAX(140, sz.width + 52);
        OS9MenuItemView *v = [[OS9MenuItemView alloc] initWithFrame:NSMakeRect(0, 0, w, 20)];
        it.view = v;
    }
}

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

#pragma mark - OS9Toast (retro theo toast.png)

static const CGFloat kToastShadow = 3;   // bóng đổ cứng (offset phải-dưới)
static const CGFloat kToastBorder = 3;   // viền đen dày
static const CGFloat kToastH = 30;       // cao thân (chưa gồm bóng)
static const CGFloat kToastPadL = 12, kToastIcon = 16, kToastGapL = 8, kToastClose = 24, kToastMaxW = 380;

@implementation OS9Toast

- (instancetype)initWithMessage:(NSString *)msg kind:(NSInteger)kind {
    if ((self = [super initWithFrame:NSZeroRect])) { _message = [msg copy]; _kind = kind; }
    return self;
}
- (BOOL)isFlipped { return YES; }

+ (NSFont *)textFont { return [OS9Theme monoFont]; }

+ (NSSize)sizeForMessage:(NSString *)msg {
    NSSize ts = [(msg ?: @"") sizeWithAttributes:@{NSFontAttributeName : [self textFont]}];
    CGFloat w = kToastPadL + kToastIcon + kToastGapL + ts.width + 8 + kToastClose;
    w = MAX(150, MIN(w, kToastMaxW));
    return NSMakeSize(w + kToastShadow, kToastH + kToastShadow);
}

- (NSColor *)fillColor {
    if (_kind == 1) return [NSColor colorWithCalibratedRed:0.64 green:0.85 blue:0.62 alpha:1]; // xanh retro
    if (_kind == 2) return [NSColor colorWithCalibratedRed:0.93 green:0.64 blue:0.64 alpha:1]; // đỏ retro
    return [NSColor colorWithCalibratedWhite:0.82 alpha:1];                                     // xám
}
- (NSString *)glyph { return _kind == 1 ? @"✓" : (_kind == 2 ? @"!" : @"i"); }

- (NSRect)bodyRect { return NSMakeRect(0, 0, self.bounds.size.width - kToastShadow, kToastH); }
- (NSRect)closeRect {
    NSRect b = [self bodyRect];
    return NSMakeRect(NSMaxX(b) - kToastClose, 0, kToastClose, b.size.height);
}

- (void)drawRect:(NSRect)dirty {
    NSRect body = [self bodyRect];
    // bóng đổ cứng (đen, lệch phải-dưới)
    [[NSColor blackColor] set];
    NSRectFill(NSMakeRect(kToastShadow, kToastShadow, body.size.width, body.size.height));
    // nền theo loại + viền đen dày, GÓC VUÔNG (retro)
    [[self fillColor] set];
    NSRectFill(body);
    [[NSColor blackColor] set];
    NSFrameRectWithWidth(NSInsetRect(body, kToastBorder / 2.0, kToastBorder / 2.0), kToastBorder);

    // icon trạng thái bên trái (đậm)
    NSDictionary *ga = @{NSFontAttributeName : [NSFont boldSystemFontOfSize:13],
                         NSForegroundColorAttributeName : [NSColor blackColor]};
    NSSize gs = [[self glyph] sizeWithAttributes:ga];
    [[self glyph] drawAtPoint:NSMakePoint(kToastPadL + (kToastIcon - gs.width) / 2,
                                          (body.size.height - gs.height) / 2) withAttributes:ga];
    // text
    NSDictionary *ta = @{NSFontAttributeName : [OS9Toast textFont], NSForegroundColorAttributeName : [NSColor blackColor]};
    CGFloat tx = kToastPadL + kToastIcon + kToastGapL;
    NSRect tr = NSMakeRect(tx, 0, NSMinX([self closeRect]) - tx - 4, body.size.height);
    NSSize ts = [(_message ?: @"") sizeWithAttributes:ta];
    [_message drawInRect:NSMakeRect(tr.origin.x, (body.size.height - ts.height) / 2, tr.size.width, ts.height)
          withAttributes:ta];
    // nút ✕ bên phải
    NSDictionary *xa = @{NSFontAttributeName : [NSFont boldSystemFontOfSize:12], NSForegroundColorAttributeName : [NSColor blackColor]};
    NSRect cr = [self closeRect];
    NSSize xs = [@"✕" sizeWithAttributes:xa];
    [@"✕" drawAtPoint:NSMakePoint(NSMidX(cr) - xs.width / 2, (body.size.height - xs.height) / 2) withAttributes:xa];
}

- (void)mouseDown:(NSEvent *)e { if (_onClose) _onClose(); }   // bấm bất kỳ -> đóng (✕ là affordance)

@end

NSTextField *OS9Label(NSString *text) {
    NSTextField *l = [NSTextField labelWithString:text ?: @""];
    l.font = [OS9Theme uiFont];
    l.textColor = [NSColor blackColor];
    l.backgroundColor = [NSColor clearColor];
    l.drawsBackground = NO;
    return l;
}

// Cell tự căn giữa text theo chiều dọc trong bounds.
@interface OS9VCenterCell : NSTextFieldCell
@end
@implementation OS9VCenterCell
- (NSRect)titleRectForBounds:(NSRect)rect {
    NSRect tr = [super titleRectForBounds:rect];
    CGFloat th = [self.attributedStringValue size].height;
    if (th > 0 && th < rect.size.height) {
        tr.origin.y = rect.origin.y + (rect.size.height - th) / 2.0;
        tr.size.height = th;
    }
    return tr;
}
- (void)drawInteriorWithFrame:(NSRect)frame inView:(NSView *)v {
    [super drawInteriorWithFrame:[self titleRectForBounds:frame] inView:v];
}
@end

NSTextField *OS9CenteredLabel(NSString *text) {
    NSTextField *l = [[NSTextField alloc] initWithFrame:NSZeroRect];
    OS9VCenterCell *cell = [[OS9VCenterCell alloc] initTextCell:text ?: @""];
    cell.bezeled = NO; cell.editable = NO; cell.selectable = NO;
    l.cell = cell;
    l.font = [OS9Theme uiFont];
    l.textColor = [NSColor blackColor];
    l.backgroundColor = [NSColor clearColor];
    l.drawsBackground = NO;
    return l;
}
