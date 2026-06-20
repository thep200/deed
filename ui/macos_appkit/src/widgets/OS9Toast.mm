#import "widgets/OS9Toast.h"
#import "theme/OS9Theme.h"

#pragma mark - OS9Toast (retro phẳng, viền nét đứt)

static const CGFloat kToastBorder = 1;   // độ dày viền line mỏng
static const CGFloat kToastH = 30;       // cao thân
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

// Nền LUÔN là màu xám (không tô theo loại).
- (NSColor *)fillColor { return [NSColor colorWithCalibratedWhite:0.82 alpha:1]; }

// Màu viền nét đứt theo loại (lấy từ assets/color.png).
- (NSColor *)accentColor {
    if (_kind == 1) return [NSColor colorWithCalibratedRed:0.29 green:0.59 blue:0.40 alpha:1]; // xanh lá
    if (_kind == 2) return [NSColor colorWithCalibratedRed:0.78 green:0.25 blue:0.22 alpha:1]; // đỏ
    return [NSColor colorWithCalibratedRed:0.42 green:0.50 blue:0.69 alpha:1];                  // xanh-xám (info)
}
- (NSString *)glyph { return _kind == 1 ? @"✓" : (_kind == 2 ? @"!" : @"i"); }

- (NSRect)closeRect {
    return NSMakeRect(NSMaxX(self.bounds) - kToastClose, 0, kToastClose, self.bounds.size.height);
}

- (void)drawRect:(NSRect)dirty {
    NSRect body = self.bounds;
    // nền phẳng xám (không bóng đổ)
    [[self fillColor] set];
    NSRectFill(body);
    // viền LINE MỎNG tô theo loại
    NSRect br = NSInsetRect(body, kToastBorder / 2.0 + 0.5, kToastBorder / 2.0 + 0.5);
    NSBezierPath *bp = [NSBezierPath bezierPathWithRect:br];
    bp.lineWidth = kToastBorder;
    [[self accentColor] set];
    [bp stroke];

    // icon trạng thái bên trái (đậm)
    NSDictionary *ga = @{NSFontAttributeName : [OS9Theme uiFontOfSize:13 bold:YES],
                         NSForegroundColorAttributeName : [NSColor blackColor]};
    NSSize gs = [[self glyph] sizeWithAttributes:ga];
    [[self glyph] drawAtPoint:NSMakePoint(kToastPadL + (kToastIcon - gs.width) / 2,
                                          (body.size.height - gs.height) / 2) withAttributes:ga];
    // text (cắt đuôi bằng … nếu dài quá)
    NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary *ta = @{NSFontAttributeName : [OS9Toast textFont],
                         NSForegroundColorAttributeName : [NSColor blackColor],
                         NSParagraphStyleAttributeName : ps};
    CGFloat tx = kToastPadL + kToastIcon + kToastGapL;
    NSRect tr = NSMakeRect(tx, 0, NSMinX([self closeRect]) - tx - 4, body.size.height);
    NSSize ts = [(_message ?: @"") sizeWithAttributes:ta];
    [_message drawInRect:NSMakeRect(tr.origin.x, (body.size.height - ts.height) / 2, tr.size.width, ts.height)
          withAttributes:ta];
    // nút ✕ bên phải
    NSDictionary *xa = @{NSFontAttributeName : [OS9Theme uiFontOfSize:12 bold:YES], NSForegroundColorAttributeName : [NSColor blackColor]};
    NSRect cr = [self closeRect];
    NSSize xs = [@"✕" sizeWithAttributes:xa];
    [@"✕" drawAtPoint:NSMakePoint(NSMidX(cr) - xs.width / 2, (body.size.height - xs.height) / 2) withAttributes:xa];
}

- (void)mouseDown:(NSEvent *)e { if (_onClose) _onClose(); }   // bấm bất kỳ -> đóng (✕ là affordance)

@end
