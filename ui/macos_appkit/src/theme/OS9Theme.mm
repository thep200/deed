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
+ (NSColor *)rowSelectionGray { static NSColor *c; if (!c) c = G(0.82); return c; } // xám nhẹ Platinum (#D1D1D1)
+ (NSColor *)titleActive { static NSColor *c; if (!c) c = G(0.80);  return c; }

static NSString *gFontName = nil;   // tên font người dùng cấu hình (truyền thẳng, vd "Monaco 9")
static CGFloat gFontSize = 11;
static NSFont *gUiFont = nil;    // cache: tránh tra cứu font mỗi lần vẽ/đo chữ
static NSFont *gMonoFont = nil;
static NSFont *gBoldUiFont = nil;   // cache: boldUiFont gọi mỗi drawRect title bar (NSFontManager đắt)

+ (void)setConfiguredFontName:(NSString *)name size:(CGFloat)size {
    gFontName = (name.length ? [name copy] : nil);
    gFontSize = (size > 0 ? size : 11);
    gUiFont = nil; gMonoFont = nil; gBoldUiFont = nil;   // đổi cấu hình -> bỏ cache để dựng lại
}

+ (NSParagraphStyle *)truncatingTailStyle {
    static NSParagraphStyle *ps;
    if (!ps) {
        NSMutableParagraphStyle *m = [[NSMutableParagraphStyle alloc] init];
        m.lineBreakMode = NSLineBreakByTruncatingTail;
        ps = [m copy];
    }
    return ps;
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

// Cùng HỌ chữ với uiFont (font cấu hình) nhưng đổi size + đậm theo vai trò (title/heading).
// Dùng cho mọi chỗ trước đây hardcode boldSystemFont -> đồng bộ font toàn app khi cấu hình.
+ (NSFont *)uiFontOfSize:(CGFloat)size bold:(BOOL)bold {
    NSFontManager *fm = [NSFontManager sharedFontManager];
    NSFont *f = [fm convertFont:[self uiFont] toSize:(size > 0 ? size : gFontSize)];
    if (bold) f = [fm convertFont:f toHaveTrait:NSBoldFontMask];   // không có bold -> giữ regular
    return f ?: [self uiFont];
}

// Đậm ở ĐÚNG size cấu hình (gFontSize) — title bar & tiêu đề màn theo size người dùng đặt.
// Cache: title bar gọi mỗi drawRect; convertFont qua NSFontManager không nên chạy lại mỗi frame.
+ (NSFont *)boldUiFont {
    if (!gBoldUiFont) gBoldUiFont = [self uiFontOfSize:gFontSize bold:YES];
    return gBoldUiFont;
}

+ (NSString *)configuredFontName { return gFontName; }   // truyền thẳng tên cấu hình (cho Scintilla)
+ (CGFloat)configuredFontSize { return gFontSize; }

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
    // Fill TOÀN BỘ bounds (không inset) để xoá sạch mép mỗi lần vẽ -> tránh viền antialias
    // cộng dồn alpha khi vẽ lại (view không layer-backed, vẽ source-over vào backing chung).
    [(pressed ? G(0.20) : [NSColor whiteColor]) set];   // nhấn: nền đen-xám (đảo)
    NSRectFill(r);
    [G(0.15) set];                                       // viền #262626
    NSFrameRectWithWidth(r, isDefault ? 2.0 : 1.0);      // viền crisp (no AA), idempotent
}

// btn-new.svg: nền #CCCCCC, góc vuông, viền #484848 1px, bevel trắng (trên-trái) / #808080 (dưới-phải).
+ (void)drawNewBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    [(pressed ? G(0.70) : G(0.80)) set]; // #CCCCCC
    NSRectFill(r);                       // fill full bounds (xem ghi chú drawLineButtonInRect)

    NSRect bz = NSInsetRect(r, 1, 1);    // bevel ngay trong viền 1px
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
    NSFrameRectWithWidth(r, isDefault ? 2.0 : 1.0);          // viền crisp (no AA), idempotent
}

+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    NSBezierPath *path = [self steppedPathInRect:NSInsetRect(r, 0.5, 0.5)];

    // nền #DDDDDD (lõm khi nhấn)
    [(pressed ? G(0.62) : [self buttonFace]) set];
    [path fill];

    // Tắt antialias cho các nét viền: stroke phủ NGUYÊN pixel (alpha 1) nên vẽ lại không
    // cộng dồn -> tránh "viền đậm dần" khi click nhanh (view không layer-backed, vẽ source-over).
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

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

    [NSGraphicsContext restoreGraphicsState];
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

// === Title bar OS9 Platinum (PROMPT_os9_titlebar_objcpp.md) ======================
// Bảng màu hex chính xác theo §"BẢNG MÀU" của prompt.
static NSColor *kFrameBorder(void) { return G(0.149); }  // #262626 — viền đen/glyph/chữ
static NSColor *kBarFill(void)     { return G(0.800); }  // #CCCCCC — nền thanh active
static NSColor *kInactiveBar(void) { return G(0.839); }  // #D6D6D6 — nền thanh inactive
static NSColor *kGripBg(void)      { return G(0.867); }  // #DDDDDD — nền pinstripe
static NSColor *kGripLight(void)   { return G(0.933); }  // #EEEEEE — cạnh trái sáng
static NSColor *kGripDark(void)    { return G(0.773); }  // #C5C5C5 — cạnh phải tối
static NSColor *kGripLine(void)    { return G(0.600); }  // #999999 — vạch ngang 1px/2px
static NSColor *kBevelHi(void)     { return [NSColor whiteColor]; }  // #FFFFFF
static NSColor *kBevelLo(void)     { return G(0.502); }  // #808080 — outer bevel tối
static NSColor *kFaceTop(void)     { return G(0.788); }  // #C9C9C9 — mặt nút (đỉnh, tối)
static NSColor *kFaceBot(void)     { return G(0.945); }  // #F1F1F1 — mặt nút (đáy, sáng)
static NSColor *kInnerHi(void)     { return [NSColor whiteColor]; }  // #FFFFFF — inner bevel sáng
static NSColor *kInnerLo(void)     { return G(0.604); }  // #9A9A9A — inner bevel tối
static NSColor *kPressTop(void)    { return G(0.208); }  // #353535 — overlay pressed (TL)
static NSColor *kPressBot(void)    { return G(0.612); }  // #9C9C9C — overlay pressed (BR)

// §2 — Khung thanh: viền 1px #262626 + nền #CCCCCC + 2 dải inner-shadow hard-edge.
// Vẽ idempotent (fill nền opaque trước -> alpha không cộng dồn giữa các drawRect).
+ (void)drawTitleBarFrameInRect:(NSRect)r {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    CGFloat W = r.size.width;
    CGFloat x0 = r.origin.x, y0 = r.origin.y;

    // Nền thân #CCCCCC (phủ toàn bộ -> reset backing cho vùng thanh).
    [kBarFill() set];
    NSRectFill(r);

    // Chỉ 1 đường kẻ ĐẬM + shadow ở MÉP DƯỚI title bar (y0 = đáy, non-flipped).
    // Bỏ viền đen 4 cạnh + inner-shadow box trước đây.
    [[kFrameBorder() colorWithAlphaComponent:0.25] set];   // shadow mờ ngay trên line
    NSRectFill(NSMakeRect(x0, y0 + 1, W, 1));
    [kFrameBorder() set];                                  // line đậm #262626 sát đáy
    NSRectFill(NSMakeRect(x0, y0, W, 1));

    [NSGraphicsContext restoreGraphicsState];
}

// Inactive — nền PHẲNG #D6D6D6, không pinstripe, không nút (theo §TRẠNG THÁI 5).
// Giữ 1 line đáy mờ để tách khỏi nội dung nhưng KHÔNG nhấn mạnh bevel.
+ (void)drawTitleBarInactiveInRect:(NSRect)r {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [kInactiveBar() set];
    NSRectFill(r);
    [[kFrameBorder() colorWithAlphaComponent:0.35] set];   // line đáy mờ
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 1));
    [NSGraphicsContext restoreGraphicsState];
}

// §3 — Dải vân grip liền mạch. r cấp x/width + chiều cao thanh (để căn giữa dọc khối 13px).
+ (void)drawTitleGripInRect:(NSRect)r {
    if (r.size.width <= 0) return;
    const CGFloat kH = 13;
    CGFloat gy = floor(r.origin.y + (r.size.height - kH) / 2);
    CGFloat gx = floor(r.origin.x), gw = floor(r.size.width);
    NSRect strip = NSMakeRect(gx, gy, gw, kH);

    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    [kGripBg() set];   NSRectFill(strip);                                  // nền #DDDDDD
    [kGripLine() set];                                                     // vân #999999 / 2px
    for (int i = 1; i <= 11; i += 2)
        NSRectFill(NSMakeRect(gx, gy + i, gw, 1));
    [kGripLight() set]; NSRectFill(NSMakeRect(gx, gy, 1, kH));             // cạnh trái #EEEEEE
    [kGripDark() set];  NSRectFill(NSMakeRect(NSMaxX(strip) - 1, gy, 1, kH)); // cạnh phải #C5C5C5

    [NSGraphicsContext restoreGraphicsState];
}

// Nút title — vẽ trực tiếp theo PIXEL trong r (góc vuông), non-flipped (y lớn = trên).
// Khung đen lớn, vạch collapse, ô vuông nhỏ zoom DÙNG CHUNG bề dày `t` -> nét đồng nhất.
// Lớp (ngoài→trong): outer bevel 1px | khung đen t px | inner bevel 1px | mặt gradient.
+ (void)drawTitleButtonInRect:(NSRect)r glyph:(int)glyph pressed:(BOOL)pressed {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    // Pixel-snap: gốc + cạnh về số nguyên pt -> sắc 1px ở cả 1x & 2x.
    CGFloat x = floor(r.origin.x), y = floor(r.origin.y);
    CGFloat s = floor(MIN(r.size.width, r.size.height));     // hộp vuông
    CGFloat R = x + s, T = y + s;                            // mép phải / mép trên
    NSColor *dark = kFrameBorder();
    CGFloat t = (s >= 15) ? 2 : 1;                           // bề dày ĐỒNG NHẤT khung/vạch/ô

    // (1) Outer bevel 1px (lõm): trên+trái = #808080, dưới+phải = #FFFFFF.
    [kBevelHi() set];                                        // L sáng (dưới+phải) trước
    NSRectFill(NSMakeRect(x, y, s, 1));                      // dưới
    NSRectFill(NSMakeRect(R - 1, y, 1, s));                  // phải
    [kBevelLo() set];                                        // L tối (trên+trái) đè lên
    NSRectFill(NSMakeRect(x, T - 1, s, 1));                  // trên
    NSRectFill(NSMakeRect(x, y, 1, s));                      // trái

    // (2) Khung đen lớn — dày t px (đậm bằng glyph).
    [dark set];
    for (int i = 0; i < (int)t; i++)
        NSFrameRect(NSMakeRect(x + 1 + i, y + 1 + i, s - 2 - 2 * i, s - 2 - 2 * i));

    // Vùng trong khung (inner bevel + mặt). Glyph vẽ ở đây -> chạm sát khung lớn.
    NSRect area = NSMakeRect(x + 1 + t, y + 1 + t, s - 2 - 2 * t, s - 2 - 2 * t);
    NSRect face = NSInsetRect(area, 1, 1);                   // chừa 1px inner bevel

    // (3) Mặt: gradient DỌC, tối ở đỉnh (#C9C9C9) -> sáng ở đáy (#F1F1F1).
    NSGradient *fg = [[NSGradient alloc] initWithStartingColor:kFaceTop() endingColor:kFaceBot()];
    [fg drawInRect:face angle:270];

    // (4) Inner bevel 1px (nổi): trên+trái = #FFFFFF, dưới+phải = #9A9A9A.
    [kInnerLo() set];                                        // L tối (dưới+phải) trước
    NSRectFill(NSMakeRect(area.origin.x, area.origin.y, area.size.width, 1));      // dưới
    NSRectFill(NSMakeRect(NSMaxX(area) - 1, area.origin.y, 1, area.size.height));  // phải
    [kInnerHi() set];                                        // L sáng (trên+trái) đè lên
    NSRectFill(NSMakeRect(area.origin.x, NSMaxY(area) - 1, area.size.width, 1));   // trên
    NSRectFill(NSMakeRect(area.origin.x, area.origin.y, 1, area.size.height));     // trái

    // (5) Glyph (#262626) — chạm sát khung lớn; nét dày t (đồng nhất khung).
    if (glyph != 0) {
        [dark set];
        CGFloat ax = area.origin.x, ay = area.origin.y, aw = area.size.width, ah = area.size.height;
        if (glyph == 1) {                        // zoom: ô nhỏ = 1/4 khung lớn (cạnh tại trung điểm)
            CGFloat hx = floor(aw / 2), hy = floor(ah / 2);
            // cạnh PHẢI ô nhỏ (dọc): mép TRÁI nét tại trung điểm ngang, lên chạm đỉnh khung.
            NSRectFill(NSMakeRect(ax + hx, ay + hy, t, ah - hy));
            // cạnh DƯỚI ô nhỏ (ngang): mép TRÊN nét tại trung điểm dọc (kéo xuống) -> ô nhỏ vuông 1/4.
            NSRectFill(NSMakeRect(ax, ay + hy - t, hx + t, t));
        } else if (glyph == 2) {                 // collapse: 2 vạch ngang GẦN nhau, căn giữa (nhích lên)
            CGFloat gap = MAX(1, t - 1);         // khe nhỏ giữa 2 vạch -> cụm sát nhau ở giữa
            CGFloat bottomMargin = ceil((ah - (2 * t + gap)) / 2.0);  // dư dồn xuống đáy -> cụm nhích lên
            CGFloat b1 = ay + bottomMargin;      // vạch dưới
            CGFloat b2 = b1 + t + gap;           // vạch trên
            NSRectFill(NSMakeRect(ax, b1, aw, t));   // chạm cả 2 cạnh trái–phải khung lớn
            NSRectFill(NSMakeRect(ax, b2, aw, t));
        }
    }

    // (6) Overlay pressed: #353535→#9C9C9C @0.8, chéo TL→BR, chỉ phủ MẶT.
    if (pressed) {
        NSGradient *ov = [[NSGradient alloc]
            initWithStartingColor:[kPressTop() colorWithAlphaComponent:0.8]
                      endingColor:[kPressBot() colorWithAlphaComponent:0.8]];
        [ov drawInRect:face angle:315];        // start TL -> end BR
    }

    [NSGraphicsContext restoreGraphicsState];
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
+ (void)drawStripedTitleInRect:(NSRect)r stripesInRect:(NSRect)stripesRect active:(BOOL)active {
    [G(0.8) set]; // #CCCCCC
    NSRectFill(r);
    NSRect band = NSInsetRect(r, 0, 3);
    [G(0.867) set]; // #DDDDDD
    NSRectFill(band);
    // Sọc chỉ kẻ trong khoảng giữa 2 cụm icon (không tràn ra dưới icon).
    CGFloat sx = MAX(NSMinX(band), NSMinX(stripesRect));
    CGFloat sw = MIN(NSMaxX(band), NSMaxX(stripesRect)) - sx;
    if (sw > 0) {
        if (active) {
            [G(0.6) set]; // #999999 — sọc ngang mỗi 2px
            for (CGFloat y = band.origin.y + 1; y < NSMaxY(band); y += 2)
                NSRectFill(NSMakeRect(sx, y, sw, 1));
        }
        // viền sáng/tối hai mép dải
        [G(0.93) set]; NSRectFill(NSMakeRect(sx, NSMaxY(band) - 1, sw, 1));
        [G(0.77) set]; NSRectFill(NSMakeRect(sx, band.origin.y, sw, 1));
    }
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
