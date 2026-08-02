#import "widgets/OS9BevelButton.h"
#import "theme/OS9Theme.h"

static const CGFloat kTitlePadX = 5;   // side inset so a truncated title never touches the bevel

// Centered + tail-truncating: a title wider than the button renders as "long titl…".
static NSParagraphStyle *CenteredTruncStyle(void) {
    static NSParagraphStyle *ps;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableParagraphStyle *m = [[NSMutableParagraphStyle alloc] init];
        m.lineBreakMode = NSLineBreakByTruncatingTail;
        m.alignment = NSTextAlignmentCenter;
        ps = [m copy];
    });
    return ps;
}

@implementation OS9BevelButton {
    BOOL _pressed;
}

- (instancetype)initWithTitle:(NSString *)title target:(id)target action:(SEL)action {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 80, 22)])) {
        _title = [title copy];
        _enabledState = YES;
        self.target = target;
        self.action = action;
        self.focusRingType = NSFocusRingTypeNone; // no focus ring (avoids sticky "bold border")
    }
    return self;
}

- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstResponder { return NO; } // no focus -> no focus border

- (void)drawRect:(NSRect)dirty {
    BOOL sunken = _pressed || _selected;   // "selected" (tab) looks pressed
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
                            NSForegroundColorAttributeName : fg,
                            NSParagraphStyleAttributeName : CenteredTruncStyle()};
    NSString *title = _title ?: @"";
    NSSize sz = [title sizeWithAttributes:attrs];
    CGFloat cw = self.bounds.size.width - (_dropdown ? 16 : 0); // leave room for arrow
    NSRect tr = NSMakeRect(kTitlePadX, floor((self.bounds.size.height - sz.height) / 2) + (sunken ? -1 : 0),
                           MAX(0, cw - 2 * kTitlePadX), sz.height);
    [title drawInRect:tr withAttributes:attrs];   // rect draw -> centered, tail-truncated with "…"
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
    [self displayIfNeeded]; // redraw "released" state NOW, before action opens modal/menu
    if (fire && self.action) [NSApp sendAction:self.action to:self.target from:self];
}

- (void)setTitle:(NSString *)title { _title = [title copy]; [self setNeedsDisplay:YES]; }
- (void)setEnabledState:(BOOL)e { _enabledState = e; [self setNeedsDisplay:YES]; }

@end
