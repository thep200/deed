#import "widgets/OS9Toast.h"
#import "theme/OS9Theme.h"

#pragma mark - OS9Toast (flat retro, dashed border)

static const CGFloat kToastBorder = 1;   // thin line border thickness
static const CGFloat kToastH = 30;       // body height
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
    return NSMakeSize(w, kToastH);
}

// Background is ALWAYS gray (not colored by kind).
- (NSColor *)fillColor { return [OS9Theme toastBg]; }

// Dashed border color by kind (from assets/color.png).
- (NSColor *)accentColor {
    if (_kind == 1) return [OS9Theme toastOk];
    if (_kind == 2) return [OS9Theme toastError];
    return [OS9Theme toastInfo];
}
- (NSString *)glyph { return _kind == 1 ? @"✓" : (_kind == 2 ? @"!" : @"i"); }

- (NSRect)closeRect {
    return NSMakeRect(NSMaxX(self.bounds) - kToastClose, 0, kToastClose, self.bounds.size.height);
}

- (void)drawRect:(NSRect)dirty {
    NSRect body = self.bounds;
    // flat gray background (no drop shadow)
    [[self fillColor] set];
    NSRectFill(body);
    // THIN LINE border colored by kind
    NSRect br = NSInsetRect(body, kToastBorder / 2.0 + 0.5, kToastBorder / 2.0 + 0.5);
    NSBezierPath *bp = [NSBezierPath bezierPathWithRect:br];
    bp.lineWidth = kToastBorder;
    [[self accentColor] set];
    [bp stroke];

    // status icon on the left. Success (kind 1) uses the vintage Platinum check; others a bold glyph.
    if (_kind == 1) {
        CGFloat s = 13;
        [OS9Theme drawCheckInRect:NSMakeRect(kToastPadL + (kToastIcon - s) / 2, (body.size.height - s) / 2, s, s)
                            color:[OS9Theme textPrimary]];
    } else {
        NSDictionary *ga = @{NSFontAttributeName : [OS9Theme uiFontOfSize:13 bold:YES],
                             NSForegroundColorAttributeName : [OS9Theme textPrimary]};
        NSSize gs = [[self glyph] sizeWithAttributes:ga];
        [[self glyph] drawAtPoint:NSMakePoint(kToastPadL + (kToastIcon - gs.width) / 2,
                                              (body.size.height - gs.height) / 2) withAttributes:ga];
    }
    // text (truncate with … if too long) — shared paragraph style
    NSDictionary *ta = @{NSFontAttributeName : [OS9Toast textFont],
                         NSForegroundColorAttributeName : [OS9Theme textPrimary],
                         NSParagraphStyleAttributeName : [OS9Theme truncatingTailStyle]};
    CGFloat tx = kToastPadL + kToastIcon + kToastGapL;
    NSRect tr = NSMakeRect(tx, 0, NSMinX([self closeRect]) - tx - 4, body.size.height);
    NSSize ts = [(_message ?: @"") sizeWithAttributes:ta];
    [_message drawInRect:NSMakeRect(tr.origin.x, (body.size.height - ts.height) / 2, tr.size.width, ts.height)
          withAttributes:ta];
    // ✕ button on the right
    NSDictionary *xa = @{NSFontAttributeName : [OS9Theme uiFontOfSize:12 bold:YES], NSForegroundColorAttributeName : [OS9Theme textPrimary]};
    NSRect cr = [self closeRect];
    NSSize xs = [@"✕" sizeWithAttributes:xa];
    [@"✕" drawAtPoint:NSMakePoint(NSMidX(cr) - xs.width / 2, (body.size.height - xs.height) / 2) withAttributes:xa];
}

- (void)mouseDown:(NSEvent *)e { if (_onClose) _onClose(); }   // click anywhere -> close (✕ is the affordance)

@end
