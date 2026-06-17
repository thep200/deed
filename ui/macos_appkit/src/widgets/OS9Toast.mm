#import "widgets/OS9Toast.h"
#import "theme/OS9Theme.h"

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
