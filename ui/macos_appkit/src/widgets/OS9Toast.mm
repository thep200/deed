#import "widgets/OS9Toast.h"
#import "theme/OS9Theme.h"

#pragma mark - OS9Toast (flat retro, dashed border)

static const CGFloat kToastBorder = 1;   // thin line border thickness
static const CGFloat kToastH = 30;       // body height (shadow sits below/right of it)
static const CGFloat kToastShadow = 3;   // Platinum drop shadow depth (bottom + right)
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
    return NSMakeSize(w + kToastShadow, kToastH + kToastShadow);   // view holds body + shadow
}

// Body = bounds minus the shadow gutter on the bottom/right edges.
- (NSRect)bodyRect {
    NSRect b = self.bounds;
    return NSMakeRect(0, 0, b.size.width - kToastShadow, b.size.height - kToastShadow);
}

// Background is ALWAYS gray (not colored by kind).
- (NSColor *)fillColor { return [NSColor colorWithCalibratedWhite:0.82 alpha:1]; }

// Dashed border color by kind (from assets/color.png).
- (NSColor *)accentColor {
    if (_kind == 1) return [NSColor colorWithCalibratedRed:0.29 green:0.59 blue:0.40 alpha:1]; // green
    if (_kind == 2) return [NSColor colorWithCalibratedRed:0.78 green:0.25 blue:0.22 alpha:1]; // red
    return [NSColor colorWithCalibratedRed:0.42 green:0.50 blue:0.69 alpha:1];                  // blue-gray (info)
}
- (NSString *)glyph { return _kind == 1 ? @"✓" : (_kind == 2 ? @"!" : @"i"); }

- (NSRect)closeRect {
    NSRect b = [self bodyRect];
    return NSMakeRect(NSMaxX(b) - kToastClose, 0, kToastClose, b.size.height);
}

- (void)drawRect:(NSRect)dirty {
    NSRect body = [self bodyRect];
    // Platinum drop shadow: solid dark band under the bottom edge + a thinner one down the right,
    // both inset by the shadow depth so the corner reads as a bevel, not a box.
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.28] set];
    NSRectFillUsingOperation(NSMakeRect(kToastShadow, NSMaxY(body), body.size.width, kToastShadow),
                             NSCompositingOperationSourceOver);
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.18] set];
    NSRectFillUsingOperation(NSMakeRect(NSMaxX(body), kToastShadow, kToastShadow, body.size.height - kToastShadow),
                             NSCompositingOperationSourceOver);
    [NSGraphicsContext restoreGraphicsState];
    // flat gray background
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
                            color:[NSColor blackColor]];
    } else {
        NSDictionary *ga = @{NSFontAttributeName : [OS9Theme uiFontOfSize:13 bold:YES],
                             NSForegroundColorAttributeName : [NSColor blackColor]};
        NSSize gs = [[self glyph] sizeWithAttributes:ga];
        [[self glyph] drawAtPoint:NSMakePoint(kToastPadL + (kToastIcon - gs.width) / 2,
                                              (body.size.height - gs.height) / 2) withAttributes:ga];
    }
    // text (truncate with … if too long) — shared paragraph style
    NSDictionary *ta = @{NSFontAttributeName : [OS9Toast textFont],
                         NSForegroundColorAttributeName : [NSColor blackColor],
                         NSParagraphStyleAttributeName : [OS9Theme truncatingTailStyle]};
    CGFloat tx = kToastPadL + kToastIcon + kToastGapL;
    NSRect tr = NSMakeRect(tx, 0, NSMinX([self closeRect]) - tx - 4, body.size.height);
    NSSize ts = [(_message ?: @"") sizeWithAttributes:ta];
    [_message drawInRect:NSMakeRect(tr.origin.x, (body.size.height - ts.height) / 2, tr.size.width, ts.height)
          withAttributes:ta];
    // ✕ button on the right
    NSDictionary *xa = @{NSFontAttributeName : [OS9Theme uiFontOfSize:12 bold:YES], NSForegroundColorAttributeName : [NSColor blackColor]};
    NSRect cr = [self closeRect];
    NSSize xs = [@"✕" sizeWithAttributes:xa];
    [@"✕" drawAtPoint:NSMakePoint(NSMidX(cr) - xs.width / 2, (body.size.height - xs.height) / 2) withAttributes:xa];
}

- (void)mouseDown:(NSEvent *)e { if (_onClose) _onClose(); }   // click anywhere -> close (✕ is the affordance)

@end
