#import "theme/OS9Theme.h"

static NSColor *G(CGFloat v) { return [NSColor colorWithCalibratedWhite:v alpha:1.0]; }

@implementation OS9Theme

// Token màu là HẰNG SỐ -> cache 1 lần (tránh cấp phát NSColor mỗi lần drawRect gọi).
// Các getter chỉ chạy trên main thread (vẽ UI) nên lazy-static an toàn.
+ (NSColor *)face        { static NSColor *c; if (!c) c = G(0.80);  return c; }   // ~#CCCCCC platinum
+ (NSColor *)buttonFace  { static NSColor *c; if (!c) c = G(0.867); return c; }   // #DDDDDD (button.svg)
+ (NSColor *)faceLight   { static NSColor *c; if (!c) c = G(0.88);  return c; }
+ (NSColor *)highlight   { static NSColor *c; if (!c) c = G(1.00);  return c; }
+ (NSColor *)shadow      { static NSColor *c; if (!c) c = G(0.53);  return c; }
+ (NSColor *)darkShadow  { static NSColor *c; if (!c) c = G(0.33);  return c; }
+ (NSColor *)frame       { static NSColor *c; if (!c) c = G(0.0);   return c; }
+ (NSColor *)windowBg    { static NSColor *c; if (!c) c = G(0.80);  return c; }
+ (NSColor *)accent      { static NSColor *c; if (!c) c = [NSColor colorWithCalibratedRed:0.20 green:0.30 blue:0.55 alpha:1.0]; return c; }
+ (NSColor *)titleActive { static NSColor *c; if (!c) c = G(0.80);  return c; }

static NSString *gFontName = nil;
static CGFloat gFontSize = 11;
static NSFont *gUiFont = nil;    // cache: tránh tra cứu font mỗi lần vẽ/đo chữ
static NSFont *gMonoFont = nil;

+ (void)setConfiguredFontName:(NSString *)name size:(CGFloat)size {
    gFontName = (name.length ? [name copy] : nil);
    gFontSize = (size > 0 ? size : 11);
    gUiFont = nil; gMonoFont = nil;   // đổi cấu hình -> bỏ cache để dựng lại
}

+ (NSFont *)uiFont {
    if (!gUiFont) {
        if (gFontName) gUiFont = [NSFont fontWithName:gFontName size:gFontSize];
        if (!gUiFont) gUiFont = [NSFont fontWithName:@"Geneva" size:gFontSize] ?: [NSFont systemFontOfSize:gFontSize];
    }
    return gUiFont;
}
+ (NSFont *)monoFont {
    if (!gMonoFont) {
        if (gFontName) gMonoFont = [NSFont fontWithName:gFontName size:gFontSize];
        if (!gMonoFont) gMonoFont = [NSFont fontWithName:@"Monaco" size:gFontSize] ?: [NSFont userFixedPitchFontOfSize:gFontSize];
    }
    return gMonoFont;
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

// 0=line (mặc định), 1=new, 2=classic
static int gButtonStyle = 0;
+ (void)setButtonStyleName:(NSString *)name {
    if ([name isEqualToString:@"classic"]) gButtonStyle = 2;
    else if ([name isEqualToString:@"new"]) gButtonStyle = 1;
    else gButtonStyle = 0; // line
}
+ (void)setClassicButtonStyle:(BOOL)classic { gButtonStyle = classic ? 2 : 1; }
+ (void)drawButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    if (gButtonStyle == 2)      [self drawBevelInRect:r pressed:pressed isDefault:isDefault];
    else if (gButtonStyle == 1) [self drawNewBevelInRect:r pressed:pressed isDefault:isDefault];
    else                        [self drawLineButtonInRect:r pressed:pressed isDefault:isDefault];
}

+ (NSColor *)buttonFGPressed:(BOOL)pressed enabled:(BOOL)enabled {
    if (pressed && gButtonStyle == 0) return [NSColor whiteColor]; // line: nền đảo tối
    return enabled ? [NSColor blackColor] : [self shadow];
}

// Retro border-line: nền phẳng trắng/sáng + viền nét đậm, góc VUÔNG. Nhấn -> đảo (nền tối).
+ (void)drawLineButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    NSRect inner = NSInsetRect(r, 0.5, 0.5);
    [(pressed ? G(0.20) : [NSColor whiteColor]) set];   // nhấn: nền đen-xám (đảo)
    NSRectFill(inner);
    [G(0.15) set];                                       // viền #262626
    NSBezierPath *p = [NSBezierPath bezierPathWithRect:inner];
    p.lineWidth = isDefault ? 2.0 : 1.0;
    [p stroke];
}

// btn-new.svg: nền #CCCCCC, góc vuông, viền #484848 1px, bevel trắng (trên-trái) / #808080 (dưới-phải).
+ (void)drawNewBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    NSRect inner = NSInsetRect(r, 0.5, 0.5);
    [(pressed ? G(0.70) : G(0.80)) set]; // #CCCCCC
    NSRectFill(inner);

    NSRect bz = NSInsetRect(inner, 1, 1);
    NSColor *tl = pressed ? G(0.5) : [NSColor whiteColor];
    NSColor *br = pressed ? [NSColor whiteColor] : G(0.5); // #808080
    CGFloat x0 = bz.origin.x, x1 = NSMaxX(bz), y0 = bz.origin.y, y1 = NSMaxY(bz);
    [tl set];
    NSRectFill(NSMakeRect(x0, y1 - 2, bz.size.width, 2));   // top (non-flipped: maxY = trên)
    NSRectFill(NSMakeRect(x0, y0, 2, bz.size.height));       // left
    [br set];
    NSRectFill(NSMakeRect(x0, y0, bz.size.width, 2));        // bottom
    NSRectFill(NSMakeRect(x1 - 2, y0, 2, bz.size.height));   // right

    [G(0.282) set]; // viền #484848
    NSBezierPath *p = [NSBezierPath bezierPathWithRect:inner];
    p.lineWidth = isDefault ? 2.0 : 1.0;
    [p stroke];
}

+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    NSBezierPath *path = [self steppedPathInRect:NSInsetRect(r, 0.5, 0.5)];

    // nền #DDDDDD (lõm khi nhấn)
    [(pressed ? G(0.62) : [self buttonFace]) set];
    [path fill];

    // bevel trong: vẽ theo PATH RĂNG CƯA (inset) nên góc — kể cả góc trái-trên — cũng có răng cưa.
    // highlight trên-trái, shadow dưới-phải; mỗi nửa cắt theo tam giác chéo.
    NSColor *tl = pressed ? [self shadow] : [NSColor whiteColor];
    NSColor *br = pressed ? [NSColor whiteColor] : [self shadow];
    NSBezierPath *inner = [self steppedPathInRect:NSInsetRect(r, 2.0, 2.0)];
    inner.lineWidth = 1.0;
    CGFloat x0 = r.origin.x, x1 = NSMaxX(r), y0 = r.origin.y, y1 = NSMaxY(r);

    [NSGraphicsContext saveGraphicsState];
    NSBezierPath *tlTri = [NSBezierPath bezierPath];     // nửa trên-trái
    [tlTri moveToPoint:NSMakePoint(x0, y0)]; [tlTri lineToPoint:NSMakePoint(x0, y1)];
    [tlTri lineToPoint:NSMakePoint(x1, y1)]; [tlTri closePath];
    [tlTri addClip];
    [tl set]; [inner stroke];
    [NSGraphicsContext restoreGraphicsState];

    [NSGraphicsContext saveGraphicsState];
    NSBezierPath *brTri = [NSBezierPath bezierPath];     // nửa dưới-phải
    [brTri moveToPoint:NSMakePoint(x0, y0)]; [brTri lineToPoint:NSMakePoint(x1, y0)];
    [brTri lineToPoint:NSMakePoint(x1, y1)]; [brTri closePath];
    [brTri addClip];
    [br set]; [inner stroke];
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
    } else if (glyph == 2) { // zoom: ô vuông nhỏ góc trên-trái (tỉ lệ theo hộp)
        [dark set];
        CGFloat g = floor(ib.size.width * 0.55);
        NSFrameRect(NSMakeRect(ib.origin.x, NSMaxY(ib) - g, g, g));
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
