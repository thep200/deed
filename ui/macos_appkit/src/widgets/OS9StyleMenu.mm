#import "widgets/OS9StyleMenu.h"
#import "theme/OS9Theme.h"

#pragma mark - OS9MenuItemView (OS9-style dropdown item)

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
    // background: selected -> blue-purple #333399 ; normal -> platinum
    [(hl ? [NSColor colorWithCalibratedRed:0.2 green:0.2 blue:0.6 alpha:1.0] : [OS9Theme buttonFace]) set];
    NSRectFill(self.bounds);
    // tick if item is selected (state On)
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
