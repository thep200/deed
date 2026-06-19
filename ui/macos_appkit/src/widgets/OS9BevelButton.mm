#import "widgets/OS9BevelButton.h"
#import "theme/OS9Theme.h"

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
    BOOL sunken = _pressed || _selected;   // "đang chọn" (tab) trông như đang nhấn
    [OS9Theme drawButtonInRect:self.bounds pressed:sunken isDefault:_isDefault];
    if (_icon) {
        NSSize is = _icon.size;
        NSRect ir = NSMakeRect(floor((self.bounds.size.width - is.width) / 2),
                               floor((self.bounds.size.height - is.height) / 2) + (sunken ? -1 : 0),
                               is.width, is.height);
        [_icon drawInRect:ir fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                 fraction:(_enabledState ? 1.0 : 0.5)];
        return;
    }
    NSColor *fg = [OS9Theme buttonFGPressed:sunken enabled:_enabledState];
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont],
                            NSForegroundColorAttributeName : fg};
    NSString *title = _title ?: @"";
    NSSize sz = [title sizeWithAttributes:attrs];
    CGFloat cw = self.bounds.size.width - (_dropdown ? 16 : 0); // chừa chỗ mũi tên
    NSPoint pt = NSMakePoint(floor((cw - sz.width) / 2),
                             floor((self.bounds.size.height - sz.height) / 2) + (sunken ? -1 : 0));
    [title drawAtPoint:pt withAttributes:attrs];
    if (_dropdown) [OS9Theme drawDropdownArrowInRect:self.bounds];
}

- (void)setSelected:(BOOL)selected {
    if (_selected != selected) { _selected = selected; [self setNeedsDisplay:YES]; }
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
