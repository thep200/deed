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
    [OS9Theme drawBevelInRect:self.bounds pressed:_pressed isDefault:_isDefault];
    NSColor *fg = _enabledState ? [NSColor blackColor] : [OS9Theme shadow];
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont],
                            NSForegroundColorAttributeName : fg};
    NSString *title = _title ?: @"";
    NSSize sz = [title sizeWithAttributes:attrs];
    // Căn giữa thật sự: drawAtPoint dùng góc dưới-trái (view không flipped).
    NSPoint pt = NSMakePoint(floor((self.bounds.size.width - sz.width) / 2),
                             floor((self.bounds.size.height - sz.height) / 2) + (_pressed ? -1 : 0));
    [title drawAtPoint:pt withAttributes:attrs];
}

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
    [OS9Theme drawBevelInRect:self.bounds pressed:_pressed isDefault:NO];
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont],
                            NSForegroundColorAttributeName : [NSColor blackColor]};
    NSString *t = self.selectedTitle;
    NSSize sz = [t sizeWithAttributes:attrs];
    [t drawAtPoint:NSMakePoint(8, floor((self.bounds.size.height - sz.height) / 2)) withAttributes:attrs];
    // mũi ▼ bên phải
    NSString *arrow = @"▾";
    NSSize asz = [arrow sizeWithAttributes:attrs];
    [arrow drawAtPoint:NSMakePoint(self.bounds.size.width - asz.width - 7,
                                   floor((self.bounds.size.height - asz.height) / 2))
        withAttributes:attrs];
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

    // 3 ô điều khiển cửa sổ kiểu OS9: close (trái), collapse + zoom (phải).
    [self drawControlBox:[self closeRect] glyph:0];
    [self drawControlBox:[self collapseRect] glyph:1];
    [self drawControlBox:[self zoomRect] glyph:2];

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

// glyph: 0=close (trống), 1=collapse (vạch ngang), 2=zoom (ô vuông nhỏ trong)
- (void)drawControlBox:(NSRect)box glyph:(int)glyph {
    [OS9Theme drawBevelInRect:box pressed:NO isDefault:NO];
    [[OS9Theme darkShadow] set];
    if (glyph == 1) {
        NSRectFill(NSMakeRect(box.origin.x + 2, NSMidY(box) - 0.5, box.size.width - 4, 1));
    } else if (glyph == 2) {
        NSFrameRect(NSInsetRect(box, 3, 3));
    }
}

// Cả 3 ô điều khiển nằm BÊN TRÁI: close, collapse (hide), zoom (mở rộng).
- (NSRect)closeRect    { return NSMakeRect(8,  (self.bounds.size.height - 13) / 2, 13, 13); }
- (NSRect)collapseRect { return NSMakeRect(26, (self.bounds.size.height - 13) / 2, 13, 13); }
- (NSRect)zoomRect     { return NSMakeRect(44, (self.bounds.size.height - 13) / 2, 13, 13); }

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (NSPointInRect(p, [self closeRect])) {
        if (_closeTarget && _closeAction) [NSApp sendAction:_closeAction to:_closeTarget from:self];
        else [self.window performClose:nil];
        return;
    }
    if (NSPointInRect(p, [self collapseRect])) { [self.window performMiniaturize:nil]; return; }
    if (NSPointInRect(p, [self zoomRect])) { [self.window performZoom:nil]; return; }
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

NSTextField *OS9Label(NSString *text) {
    NSTextField *l = [NSTextField labelWithString:text ?: @""];
    l.font = [OS9Theme uiFont];
    l.textColor = [NSColor blackColor];
    l.backgroundColor = [NSColor clearColor];
    l.drawsBackground = NO;
    return l;
}
