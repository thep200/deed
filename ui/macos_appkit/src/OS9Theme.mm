#import "OS9Theme.h"

static NSColor *G(CGFloat v) { return [NSColor colorWithCalibratedWhite:v alpha:1.0]; }

@implementation OS9Theme

+ (NSColor *)face        { return G(0.80); }   // ~#CCCCCC platinum
+ (NSColor *)faceLight   { return G(0.88); }
+ (NSColor *)highlight   { return G(1.00); }
+ (NSColor *)shadow      { return G(0.53); }
+ (NSColor *)darkShadow  { return G(0.33); }
+ (NSColor *)frame       { return G(0.0); }
+ (NSColor *)windowBg    { return G(0.80); }
+ (NSColor *)accent      { return [NSColor colorWithCalibratedRed:0.20 green:0.30 blue:0.55 alpha:1.0]; }
+ (NSColor *)titleActive { return G(0.80); }

+ (NSFont *)uiFont {
    // OS9 dùng Charcoal/Geneva; thay bằng font hệ thống cỡ nhỏ (chưa nhúng Charcoal — phase sau).
    return [NSFont fontWithName:@"Geneva" size:11] ?: [NSFont systemFontOfSize:11];
}
+ (NSFont *)monoFont {
    return [NSFont fontWithName:@"Monaco" size:11] ?: [NSFont userFixedPitchFontOfSize:11];
}

+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    NSRect inner = NSInsetRect(r, 0.5, 0.5);

    // mặt nút
    [(pressed ? [self shadow] : [self face]) set];
    NSRectFill(inner);

    NSColor *tl = pressed ? [self darkShadow] : [self highlight];
    NSColor *br = pressed ? [self highlight] : [self shadow];

    // highlight trên-trái
    [tl set];
    NSRectFill(NSMakeRect(inner.origin.x, NSMaxY(inner) - 1, inner.size.width, 1)); // top
    NSRectFill(NSMakeRect(inner.origin.x, inner.origin.y, 1, inner.size.height));    // left
    // shadow dưới-phải
    [br set];
    NSRectFill(NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, 1));      // bottom
    NSRectFill(NSMakeRect(NSMaxX(inner) - 1, inner.origin.y, 1, inner.size.height));  // right

    // viền đen ngoài
    [[self frame] set];
    NSBezierPath *p = [NSBezierPath bezierPathWithRect:inner];
    p.lineWidth = 1.0;
    [p stroke];

    if (isDefault) { // nút default: viền đậm thêm 1 lớp
        [[self frame] set];
        NSBezierPath *d = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 1.5, 1.5)];
        d.lineWidth = 1.0;
        [d stroke];
    }
}

+ (void)drawInsetInRect:(NSRect)r {
    NSRect inner = NSInsetRect(r, 0.5, 0.5);
    [[NSColor whiteColor] set];
    NSRectFill(inner);
    // tối trên-trái (sunken)
    [[self shadow] set];
    NSRectFill(NSMakeRect(inner.origin.x, NSMaxY(inner) - 1, inner.size.width, 1));
    NSRectFill(NSMakeRect(inner.origin.x, inner.origin.y, 1, inner.size.height));
    // sáng dưới-phải
    [[self highlight] set];
    NSRectFill(NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, 1));
    NSRectFill(NSMakeRect(NSMaxX(inner) - 1, inner.origin.y, 1, inner.size.height));
    [[self frame] set];
    NSBezierPath *p = [NSBezierPath bezierPathWithRect:inner];
    [p stroke];
}

+ (void)drawStripedTitleInRect:(NSRect)r active:(BOOL)active {
    // Title bar phẳng màu platinum (đã bỏ kẻ sọc trắng + đường kẻ đen theo yêu cầu).
    [[self face] set];
    NSRectFill(r);
    (void)active;
}

@end
