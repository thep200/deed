#import "widgets/OS9TitleBar.h"
#import "theme/OS9Theme.h"

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
