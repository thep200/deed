#import "widgets/OS9PopupButton.h"
#import "widgets/OS9Dropdown.h"
#import "theme/OS9Theme.h"

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
    // long names -> truncate with "…" (full name in toolTip); shared paragraph style (no per-draw alloc)
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont],
                            NSForegroundColorAttributeName : [OS9Theme buttonFGPressed:_pressed enabled:YES],
                            NSParagraphStyleAttributeName : [OS9Theme truncatingTailStyle]};
    NSString *t = self.selectedTitle;
    NSSize sz = [t sizeWithAttributes:attrs];
    CGFloat tx = 7, tw = self.bounds.size.width - tx - 16;   // leave room for ▾ arrow on the right
    [t drawInRect:NSMakeRect(tx, floor((self.bounds.size.height - sz.height) / 2), tw, sz.height)
        withAttributes:attrs];
    [OS9Theme drawDropdownArrowInRect:self.bounds]; // ▾ + divider per dropdown.svg
}

- (void)mouseDown:(NSEvent *)e {
    _pressed = YES; [self setNeedsDisplay:YES]; [self displayIfNeeded];
    if (self.onClick) {
        self.onClick();          // owner decides (e.g. load RPC over network then openMenu)
    } else {
        [self openMenu];
    }
    _pressed = NO; [self setNeedsDisplay:YES];   // overlay is modeless -> release button immediately
}

- (void)openMenu {
    __weak OS9PopupButton *ws = self;
    OS9ShowDropdown(_itemTitles, _selectedIndex, self, ^(NSInteger idx) {
        OS9PopupButton *s = ws; if (!s) return;
        s.selectedIndex = idx;
        [s setNeedsDisplay:YES];
        if (s.action) [NSApp sendAction:s.action to:s.target from:s];
    });
}

@end
