#import "OS9Theme.h"

static NSColor *G(CGFloat v) { return [NSColor colorWithCalibratedWhite:v alpha:1.0]; }

@implementation OS9Theme

+ (NSColor *)face        { return G(0.80); }   // ~#CCCCCC platinum
+ (NSColor *)buttonFace  { return G(0.867); }  // #DDDDDD (button.svg)
+ (NSColor *)faceLight   { return G(0.88); }
+ (NSColor *)highlight   { return G(1.00); }
+ (NSColor *)shadow      { return G(0.53); }
+ (NSColor *)darkShadow  { return G(0.33); }
+ (NSColor *)frame       { return G(0.0); }
+ (NSColor *)windowBg    { return G(0.80); }
+ (NSColor *)accent      { return [NSColor colorWithCalibratedRed:0.20 green:0.30 blue:0.55 alpha:1.0]; }
+ (NSColor *)titleActive { return G(0.80); }

static NSString *gFontName = nil;
static CGFloat gFontSize = 11;

+ (void)setConfiguredFontName:(NSString *)name size:(CGFloat)size {
    gFontName = (name.length ? [name copy] : nil);
    gFontSize = (size > 0 ? size : 11);
}

+ (NSFont *)uiFont {
    if (gFontName) { NSFont *f = [NSFont fontWithName:gFontName size:gFontSize]; if (f) return f; }
    return [NSFont fontWithName:@"Geneva" size:gFontSize] ?: [NSFont systemFontOfSize:gFontSize];
}
+ (NSFont *)monoFont {
    if (gFontName) { NSFont *f = [NSFont fontWithName:gFontName size:gFontSize]; if (f) return f; }
    return [NSFont fontWithName:@"Monaco" size:gFontSize] ?: [NSFont userFixedPitchFontOfSize:gFontSize];
}

// Nút góc bo pixel (theo button.svg): các góc bị cắt 2 bậc x 2px.
+ (NSBezierPath *)steppedPathInRect:(NSRect)r {
    CGFloat x0 = r.origin.x, x1 = NSMaxX(r), y0 = r.origin.y, y1 = NSMaxY(r);
    NSPoint v[] = {
        {x1 - 4, y1}, {x0 + 4, y1}, {x0 + 4, y1 - 2}, {x0 + 2, y1 - 2}, {x0 + 2, y1 - 4},
        {x0, y1 - 4}, {x0, y0 + 4}, {x0 + 2, y0 + 4}, {x0 + 2, y0 + 2}, {x0 + 4, y0 + 2},
        {x0 + 4, y0}, {x1 - 4, y0}, {x1 - 4, y0 + 2}, {x1 - 2, y0 + 2}, {x1 - 2, y0 + 4},
        {x1, y0 + 4}, {x1, y1 - 4}, {x1 - 2, y1 - 4}, {x1 - 2, y1 - 2}, {x1 - 4, y1 - 2},
    };
    NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:v[0]];
    for (int i = 1; i < 20; i++) [p lineToPoint:v[i]];
    [p closePath];
    return p;
}

+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    NSBezierPath *path = [self steppedPathInRect:NSInsetRect(r, 0.5, 0.5)];

    // nền #DDDDDD (lõm khi nhấn)
    [(pressed ? G(0.62) : [self buttonFace]) set];
    [path fill];

    // bevel trong: highlight trên-trái, shadow dưới-phải (đảo khi nhấn)
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    NSColor *tl = pressed ? [self shadow] : [NSColor whiteColor];
    NSColor *br = pressed ? [NSColor whiteColor] : [self shadow];
    CGFloat x0 = r.origin.x, x1 = NSMaxX(r), y0 = r.origin.y, y1 = NSMaxY(r);
    [tl set];
    NSRectFill(NSMakeRect(x0, y1 - 3, r.size.width, 2));   // top
    NSRectFill(NSMakeRect(x0 + 1, y0, 2, r.size.height));   // left
    [br set];
    NSRectFill(NSMakeRect(x0, y0 + 1, r.size.width, 2));    // bottom
    NSRectFill(NSMakeRect(x1 - 3, y0, 2, r.size.height));   // right
    [NSGraphicsContext restoreGraphicsState];

    // viền đen (2px theo svg; nút default đậm hơn)
    [[self frame] set];
    path.lineWidth = isDefault ? 2.4 : 1.4;
    [path stroke];
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

// Ô điều khiển kiểu Mac (theo *_box.svg): khung lõm ngoài + hộp gradient bevel + glyph.
+ (void)drawMacControlBox:(NSRect)r glyph:(int)glyph {
    // Khung ngoài lõm: tối (#808080) trên-trái, sáng (trắng) dưới-phải.
    [G(0.5) set];
    NSRectFill(NSMakeRect(r.origin.x, NSMaxY(r) - 1, r.size.width, 1));  // top
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, 1, r.size.height));    // left
    [[NSColor whiteColor] set];
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 1));      // bottom
    NSRectFill(NSMakeRect(NSMaxX(r) - 1, r.origin.y, 1, r.size.height));  // right

    // Hộp trong: gradient #9A9A9A -> #F1F1F1 (trên-trái -> dưới-phải).
    NSRect ib = NSInsetRect(r, 2, 2);
    NSGradient *grad = [[NSGradient alloc] initWithStartingColor:G(0.604) endingColor:G(0.945)];
    [grad drawInRect:ib angle:-45];

    // bevel hộp trong: trắng trên-trái, xám dưới-phải
    [[NSColor whiteColor] set];
    NSRectFill(NSMakeRect(ib.origin.x, NSMaxY(ib) - 1, ib.size.width, 1));
    NSRectFill(NSMakeRect(ib.origin.x, ib.origin.y, 1, ib.size.height));
    [G(0.5) set];
    NSRectFill(NSMakeRect(ib.origin.x, ib.origin.y, ib.size.width, 1));
    NSRectFill(NSMakeRect(NSMaxX(ib) - 1, ib.origin.y, 1, ib.size.height));

    // viền hộp #262626
    NSColor *dark = G(0.15);
    [dark set];
    NSFrameRect(ib);

    // glyph
    if (glyph == 1) { // collapse: vạch ngang giữa
        [dark set];
        NSRectFill(NSMakeRect(ib.origin.x, floor(NSMidY(ib)) - 1, ib.size.width, 2));
    } else if (glyph == 2) { // zoom: ô vuông nhỏ góc trên-trái
        [dark set];
        NSFrameRect(NSMakeRect(ib.origin.x, NSMaxY(ib) - 6, 6, 6));
    }
}

// Dropdown: vạch ngăn dọc + mũi tên 2 CHIỀU (▲ trên / ▼ dưới) ở mép phải (theo dropdown.svg).
+ (void)drawDropdownArrowInRect:(NSRect)r {
    CGFloat bw = 16;                          // vùng mũi tên bên phải
    CGFloat sx = NSMaxX(r) - bw;
    [G(0.5) set];                             // vạch ngăn #808080
    NSRectFill(NSMakeRect(sx, r.origin.y + 3, 1, r.size.height - 6));
    [[NSColor whiteColor] set];
    NSRectFill(NSMakeRect(sx + 1, r.origin.y + 3, 1, r.size.height - 6));

    CGFloat cx = sx + bw / 2 + 0.5, cy = NSMidY(r), s = 2.5, gap = 1.5;
    [G(0.15) set];                            // #262626
    // ▲ phía trên (apex hướng lên — non-flipped: y lớn = trên)
    NSBezierPath *up = [NSBezierPath bezierPath];
    [up moveToPoint:NSMakePoint(cx, cy + gap + s)];
    [up lineToPoint:NSMakePoint(cx - s, cy + gap)];
    [up lineToPoint:NSMakePoint(cx + s, cy + gap)];
    [up closePath];
    [up fill];
    // ▼ phía dưới
    NSBezierPath *dn = [NSBezierPath bezierPath];
    [dn moveToPoint:NSMakePoint(cx, cy - gap - s)];
    [dn lineToPoint:NSMakePoint(cx - s, cy - gap)];
    [dn lineToPoint:NSMakePoint(cx + s, cy - gap)];
    [dn closePath];
    [dn fill];
}

// Title bar kẻ sọc ngang Platinum theo window.svg (#CCCCCC nền, dải #DDDDDD + sọc #999999/2px).
+ (void)drawStripedTitleInRect:(NSRect)r active:(BOOL)active {
    [G(0.8) set]; // #CCCCCC
    NSRectFill(r);
    NSRect band = NSInsetRect(r, 0, 3);
    [G(0.867) set]; // #DDDDDD
    NSRectFill(band);
    if (active) {
        [G(0.6) set]; // #999999 — sọc ngang mỗi 2px
        for (CGFloat y = band.origin.y + 1; y < NSMaxY(band); y += 2)
            NSRectFill(NSMakeRect(band.origin.x, y, band.size.width, 1));
    }
    // viền sáng/tối hai mép dải
    [G(0.93) set]; NSRectFill(NSMakeRect(band.origin.x, NSMaxY(band) - 1, band.size.width, 1));
    [G(0.77) set]; NSRectFill(NSMakeRect(band.origin.x, band.origin.y, band.size.width, 1));
}

// Góc răng cưa nhỏ: 3 bậc x1px ở mỗi góc (cho ô input retro).
+ (NSBezierPath *)serratedPathInRect:(NSRect)r {
    CGFloat x0 = r.origin.x, x1 = NSMaxX(r), y0 = r.origin.y, y1 = NSMaxY(r);
    const CGFloat s = 1.0; const int n = 3; const CGFloat c = s * n; // 3px góc
    NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(x0 + c, y1)];
    [p lineToPoint:NSMakePoint(x1 - c, y1)];                 // cạnh trên
    for (int i = 0; i < n; i++) {                            // góc trên-phải răng cưa
        [p lineToPoint:NSMakePoint(x1 - c + (i + 1) * s, y1 - i * s)];
        [p lineToPoint:NSMakePoint(x1 - c + (i + 1) * s, y1 - (i + 1) * s)];
    }
    [p lineToPoint:NSMakePoint(x1, y0 + c)];                 // cạnh phải
    for (int i = 0; i < n; i++) {                            // góc dưới-phải
        [p lineToPoint:NSMakePoint(x1 - i * s, y0 + c - (i + 1) * s)];
        [p lineToPoint:NSMakePoint(x1 - (i + 1) * s, y0 + c - (i + 1) * s)];
    }
    [p lineToPoint:NSMakePoint(x0 + c, y0)];                 // cạnh dưới
    for (int i = 0; i < n; i++) {                            // góc dưới-trái
        [p lineToPoint:NSMakePoint(x0 + c - (i + 1) * s, y0 + i * s)];
        [p lineToPoint:NSMakePoint(x0 + c - (i + 1) * s, y0 + (i + 1) * s)];
    }
    [p lineToPoint:NSMakePoint(x0, y1 - c)];                 // cạnh trái
    for (int i = 0; i < n; i++) {                            // góc trên-trái
        [p lineToPoint:NSMakePoint(x0 + i * s, y1 - c + (i + 1) * s)];
        [p lineToPoint:NSMakePoint(x0 + (i + 1) * s, y1 - c + (i + 1) * s)];
    }
    [p closePath];
    return p;
}

@end
