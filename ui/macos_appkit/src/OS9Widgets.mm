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
    NSColor *fg = _enabledState ? [NSColor blackColor] : [OS9Theme shadow];
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
                            NSForegroundColorAttributeName : [NSColor blackColor]};
    NSString *t = self.selectedTitle;
    NSSize sz = [t sizeWithAttributes:attrs];
    [t drawAtPoint:NSMakePoint(7, floor((self.bounds.size.height - sz.height) / 2)) withAttributes:attrs];
    [OS9Theme drawDropdownArrowInRect:self.bounds]; // ▾ + vạch ngăn theo dropdown.svg
}

- (void)mouseDown:(NSEvent *)e {
    _pressed = YES; [self setNeedsDisplay:YES]; [self displayIfNeeded];
    NSMenu *m = [[NSMenu alloc] init];
    NSInteger i = 0;
    for (NSString *t in _itemTitles) {
        NSMenuItem *it = [m addItemWithTitle:t action:@selector(menuPicked:) keyEquivalent:@""];
        it.target = self;
        it.tag = i++;
        it.state = (it.tag == _selectedIndex) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    OS9StyleMenu(m);
    [m popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, 0) inView:self];
    _pressed = NO; [self setNeedsDisplay:YES];
}

- (void)menuPicked:(NSMenuItem *)item {
    _selectedIndex = item.tag;
    [self setNeedsDisplay:YES];
    if (self.action) [NSApp sendAction:self.action to:self.target from:self];
}

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
- (NSRect)closeRect    { return NSMakeRect(8, (self.bounds.size.height - 14) / 2, 14, 14); }
- (NSRect)zoomRect     { return NSMakeRect(self.bounds.size.width - 8 - 14, (self.bounds.size.height - 14) / 2, 14, 14); }
- (NSRect)collapseRect { return NSMakeRect(self.bounds.size.width - 8 - 14 - 18, (self.bounds.size.height - 14) / 2, 14, 14); }

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (NSPointInRect(p, [self closeRect])) {
        if (_closeTarget && _closeAction) [NSApp sendAction:_closeAction to:_closeTarget from:self];
        else [self.window performClose:nil];
        return;
    }
    if (NSPointInRect(p, [self collapseRect])) { [self.window performMiniaturize:nil]; return; }
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
        CGFloat rOut = size * 0.48, rIn = size * 0.34;
        int teeth = 8, steps = teeth * 2;
        NSBezierPath *gear = [NSBezierPath bezierPath];
        for (int i = 0; i <= steps; i++) {
            CGFloat ang = (CGFloat)i / steps * 2 * M_PI;
            CGFloat rad = (i % 2 == 0) ? rOut : rIn;
            NSPoint pt = NSMakePoint(cx + rad * cos(ang), cy + rad * sin(ang));
            if (i == 0) [gear moveToPoint:pt]; else [gear lineToPoint:pt];
        }
        [gear closePath];
        // lỗ trục ở giữa (even-odd -> đục lỗ)
        CGFloat hr = size * 0.16;
        [gear appendBezierPath:[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - hr, cy - hr, hr * 2, hr * 2)]];
        gear.windingRule = NSWindingRuleEvenOdd;
        [[NSColor blackColor] set];
        [gear fill];
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

NSTextField *OS9Label(NSString *text) {
    NSTextField *l = [NSTextField labelWithString:text ?: @""];
    l.font = [OS9Theme uiFont];
    l.textColor = [NSColor blackColor];
    l.backgroundColor = [NSColor clearColor];
    l.drawsBackground = NO;
    return l;
}
