#import "theme/OS9Theme.h"

static NSColor *G(CGFloat v) { return [NSColor colorWithCalibratedWhite:v alpha:1.0]; }

@implementation OS9Theme

// Color tokens are CONSTANT -> cache once (avoids allocating NSColor on every drawRect).
// Getters run only on the main thread (UI drawing), so lazy-static is safe.
+ (NSColor *)face        { static NSColor *c; if (!c) c = G(0.80);  return c; }   // ~#CCCCCC platinum
+ (NSColor *)buttonFace  { static NSColor *c; if (!c) c = G(0.867); return c; }   // #DDDDDD (button.svg)
+ (NSColor *)faceLight   { static NSColor *c; if (!c) c = G(0.88);  return c; }
+ (NSColor *)highlight   { static NSColor *c; if (!c) c = G(1.00);  return c; }
+ (NSColor *)shadow      { static NSColor *c; if (!c) c = G(0.53);  return c; }
+ (NSColor *)darkShadow  { static NSColor *c; if (!c) c = G(0.33);  return c; }
+ (NSColor *)frame       { static NSColor *c; if (!c) c = G(0.0);   return c; }
+ (NSColor *)windowBg    { static NSColor *c; if (!c) c = G(0.80);  return c; }
+ (NSColor *)accent      { static NSColor *c; if (!c) c = [NSColor colorWithCalibratedRed:0.20 green:0.30 blue:0.55 alpha:1.0]; return c; }
+ (NSColor *)rowSelectionGray { static NSColor *c; if (!c) c = G(0.82); return c; } // subtle Platinum gray (#D1D1D1)
+ (NSColor *)titleActive { static NSColor *c; if (!c) c = G(0.80);  return c; }

static NSString *gFontName = nil;   // user-configured font name (passed through, e.g. "Monaco 9")
static CGFloat gFontSize = 11;
static NSFont *gUiFont = nil;    // cache: avoids font lookup on every draw/measure
static NSFont *gMonoFont = nil;
static NSFont *gBoldUiFont = nil;   // cache: boldUiFont is called on every title bar drawRect (NSFontManager is costly)

+ (void)setConfiguredFontName:(NSString *)name size:(CGFloat)size {
    gFontName = (name.length ? [name copy] : nil);
    gFontSize = (size > 0 ? size : 11);
    gUiFont = nil; gMonoFont = nil; gBoldUiFont = nil;   // config changed -> drop cache to rebuild
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

// Same FAMILY as uiFont (configured font) but role-specific size + bold (title/heading).
// Used everywhere that previously hardcoded boldSystemFont -> app-wide font sync when configured.
+ (NSFont *)uiFontOfSize:(CGFloat)size bold:(BOOL)bold {
    NSFontManager *fm = [NSFontManager sharedFontManager];
    NSFont *f = [fm convertFont:[self uiFont] toSize:(size > 0 ? size : gFontSize)];
    if (bold) f = [fm convertFont:f toHaveTrait:NSBoldFontMask];   // no bold available -> keep regular
    return f ?: [self uiFont];
}

// Bold at the EXACT configured size (gFontSize) — title bar & screen titles follow user-set size.
// Cache: title bar calls this on every drawRect; convertFont via NSFontManager should not rerun per frame.
+ (NSFont *)boldUiFont {
    if (!gBoldUiFont) gBoldUiFont = [self uiFontOfSize:gFontSize bold:YES];
    return gBoldUiFont;
}

+ (NSString *)configuredFontName { return gFontName; }   // pass through configured name (for Scintilla)
+ (CGFloat)configuredFontSize { return gFontSize; }

// Pixel rounded button (per button.svg): corners cut in 2 steps x 2px.
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

// 0=line (default), 1=new, 2=classic
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
    if (pressed && gButtonStyle == 0) return [NSColor whiteColor]; // line: inverted dark fill
    return enabled ? [NSColor blackColor] : [self shadow];
}

// Retro border-line: flat white/light fill + bold stroked border, SQUARE corners. Pressed -> inverted (dark fill).
+ (void)drawLineButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    // Fill the WHOLE bounds (no inset) to fully clear edges on each draw -> avoids antialiased border
    // accumulating alpha on redraw (view is not layer-backed, draws source-over into shared backing).
    [(pressed ? G(0.20) : [NSColor whiteColor]) set];   // pressed: dark-gray fill (inverted)
    NSRectFill(r);
    [G(0.15) set];                                       // #262626 border
    NSFrameRectWithWidth(r, isDefault ? 2.0 : 1.0);      // crisp border (no AA), idempotent
}

// btn-new.svg: #CCCCCC fill, square corners, #484848 1px border, white (top-left) / #808080 (bottom-right) bevel.
+ (void)drawNewBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    [(pressed ? G(0.70) : G(0.80)) set]; // #CCCCCC
    NSRectFill(r);                       // fill full bounds (see drawLineButtonInRect note)

    NSRect bz = NSInsetRect(r, 1, 1);    // bevel just inside the 1px border
    NSColor *tl = pressed ? G(0.5) : [NSColor whiteColor];
    NSColor *br = pressed ? [NSColor whiteColor] : G(0.5); // #808080
    CGFloat x0 = bz.origin.x, x1 = NSMaxX(bz), y0 = bz.origin.y, y1 = NSMaxY(bz);
    [tl set];
    NSRectFill(NSMakeRect(x0, y1 - 2, bz.size.width, 2));   // top (non-flipped: maxY = top)
    NSRectFill(NSMakeRect(x0, y0, 2, bz.size.height));       // left
    [br set];
    NSRectFill(NSMakeRect(x0, y0, bz.size.width, 2));        // bottom
    NSRectFill(NSMakeRect(x1 - 2, y0, 2, bz.size.height));   // right

    [G(0.282) set]; // #484848 border
    NSFrameRectWithWidth(r, isDefault ? 2.0 : 1.0);          // crisp border (no AA), idempotent
}

+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    NSBezierPath *path = [self steppedPathInRect:NSInsetRect(r, 0.5, 0.5)];

    // #DDDDDD fill (sunken when pressed)
    [(pressed ? G(0.62) : [self buttonFace]) set];
    [path fill];

    // Disable antialias for border strokes: stroke covers WHOLE pixels (alpha 1) so redraw doesn't
    // accumulate -> avoids "border darkening" on rapid clicks (view not layer-backed, draws source-over).
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    // Inner bevel: drawn along the SERRATED PATH (inset) so corners — even top-left — are serrated too.
    // highlight top-left, shadow bottom-right; each half clipped by a diagonal triangle.
    NSColor *tl = pressed ? [self shadow] : [NSColor whiteColor];
    NSColor *br = pressed ? [NSColor whiteColor] : [self shadow];
    NSBezierPath *inner = [self steppedPathInRect:NSInsetRect(r, 2.0, 2.0)];
    inner.lineWidth = 1.0;
    CGFloat x0 = r.origin.x, x1 = NSMaxX(r), y0 = r.origin.y, y1 = NSMaxY(r);

    [NSGraphicsContext saveGraphicsState];
    NSBezierPath *tlTri = [NSBezierPath bezierPath];     // top-left half
    [tlTri moveToPoint:NSMakePoint(x0, y0)]; [tlTri lineToPoint:NSMakePoint(x0, y1)];
    [tlTri lineToPoint:NSMakePoint(x1, y1)]; [tlTri closePath];
    [tlTri addClip];
    [tl set]; [inner stroke];
    [NSGraphicsContext restoreGraphicsState];

    [NSGraphicsContext saveGraphicsState];
    NSBezierPath *brTri = [NSBezierPath bezierPath];     // bottom-right half
    [brTri moveToPoint:NSMakePoint(x0, y0)]; [brTri lineToPoint:NSMakePoint(x1, y0)];
    [brTri lineToPoint:NSMakePoint(x1, y1)]; [brTri closePath];
    [brTri addClip];
    [br set]; [inner stroke];
    [NSGraphicsContext restoreGraphicsState];

    // black border (2px per svg; default button is bolder)
    [[self frame] set];
    path.lineWidth = isDefault ? 2.4 : 1.4;
    [path stroke];

    [NSGraphicsContext restoreGraphicsState];
}

+ (void)drawInsetInRect:(NSRect)r {
    NSRect inner = NSInsetRect(r, 0.5, 0.5);
    [[NSColor whiteColor] set];
    NSRectFill(inner);
    // dark top-left (sunken)
    [[self shadow] set];
    NSRectFill(NSMakeRect(inner.origin.x, NSMaxY(inner) - 1, inner.size.width, 1));
    NSRectFill(NSMakeRect(inner.origin.x, inner.origin.y, 1, inner.size.height));
    // light bottom-right
    [[self highlight] set];
    NSRectFill(NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, 1));
    NSRectFill(NSMakeRect(NSMaxX(inner) - 1, inner.origin.y, 1, inner.size.height));
    [[self frame] set];
    NSBezierPath *p = [NSBezierPath bezierPathWithRect:inner];
    [p stroke];
}

// Mac-style control box (per *_box.svg): outer sunken frame + bevel gradient box + glyph.
+ (void)drawMacControlBox:(NSRect)r glyph:(int)glyph {
    // Outer sunken frame: dark (#808080) top-left, light (white) bottom-right.
    [G(0.5) set];
    NSRectFill(NSMakeRect(r.origin.x, NSMaxY(r) - 1, r.size.width, 1));  // top
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, 1, r.size.height));    // left
    [[NSColor whiteColor] set];
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 1));      // bottom
    NSRectFill(NSMakeRect(NSMaxX(r) - 1, r.origin.y, 1, r.size.height));  // right

    // Inner box: gradient #9A9A9A -> #F1F1F1 (top-left -> bottom-right).
    NSRect ib = NSInsetRect(r, 2, 2);
    NSGradient *grad = [[NSGradient alloc] initWithStartingColor:G(0.604) endingColor:G(0.945)];
    [grad drawInRect:ib angle:-45];

    // inner box bevel: white top-left, gray bottom-right
    [[NSColor whiteColor] set];
    NSRectFill(NSMakeRect(ib.origin.x, NSMaxY(ib) - 1, ib.size.width, 1));
    NSRectFill(NSMakeRect(ib.origin.x, ib.origin.y, 1, ib.size.height));
    [G(0.5) set];
    NSRectFill(NSMakeRect(ib.origin.x, ib.origin.y, ib.size.width, 1));
    NSRectFill(NSMakeRect(NSMaxX(ib) - 1, ib.origin.y, 1, ib.size.height));

    // box border #262626
    NSColor *dark = G(0.15);
    [dark set];
    NSFrameRect(ib);

    // glyph
    if (glyph == 1) { // collapse: middle horizontal bar
        [dark set];
        NSRectFill(NSMakeRect(ib.origin.x, floor(NSMidY(ib)) - 1, ib.size.width, 2));
    } else if (glyph == 2) { // zoom: small square top-left (scaled to box)
        [dark set];
        CGFloat g = floor(ib.size.width * 0.55);
        NSFrameRect(NSMakeRect(ib.origin.x, NSMaxY(ib) - g, g, g));
    }
}

// === OS9 Platinum title bar (PROMPT_os9_titlebar_objcpp.md) ======================
// Exact hex palette per the prompt's §"COLOR PALETTE".
static NSColor *kFrameBorder(void) { return G(0.149); }  // #262626 — black border/glyph/text
static NSColor *kBarFill(void)     { return G(0.800); }  // #CCCCCC — active bar background
static NSColor *kInactiveBar(void) { return G(0.839); }  // #D6D6D6 — inactive bar background
static NSColor *kGripBg(void)      { return G(0.867); }  // #DDDDDD — pinstripe background
static NSColor *kGripLight(void)   { return G(0.933); }  // #EEEEEE — light left edge
static NSColor *kGripDark(void)    { return G(0.773); }  // #C5C5C5 — dark right edge
static NSColor *kGripLine(void)    { return G(0.600); }  // #999999 — horizontal line 1px/2px
static NSColor *kBevelHi(void)     { return [NSColor whiteColor]; }  // #FFFFFF
static NSColor *kBevelLo(void)     { return G(0.502); }  // #808080 — dark outer bevel
static NSColor *kFaceTop(void)     { return G(0.788); }  // #C9C9C9 — button face (top, dark)
static NSColor *kFaceBot(void)     { return G(0.945); }  // #F1F1F1 — button face (bottom, light)
static NSColor *kInnerHi(void)     { return [NSColor whiteColor]; }  // #FFFFFF — light inner bevel
static NSColor *kInnerLo(void)     { return G(0.604); }  // #9A9A9A — dark inner bevel
static NSColor *kPressTop(void)    { return G(0.208); }  // #353535 — pressed overlay (TL)
static NSColor *kPressBot(void)    { return G(0.612); }  // #9C9C9C — pressed overlay (BR)

// §2 — Bar frame: 1px #262626 border + #CCCCCC fill + 2 hard-edge inner-shadow bands.
// Idempotent draw (fill opaque background first -> alpha doesn't accumulate across drawRects).
+ (void)drawTitleBarFrameInRect:(NSRect)r {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    CGFloat W = r.size.width;
    CGFloat x0 = r.origin.x, y0 = r.origin.y;

    // Body fill #CCCCCC (covers all -> resets backing for the bar area).
    [kBarFill() set];
    NSRectFill(r);

    // Only 1 BOLD line + shadow at the BOTTOM edge of the title bar (y0 = bottom, non-flipped).
    // Dropped the previous 4-edge black border + inner-shadow box.
    [[kFrameBorder() colorWithAlphaComponent:0.25] set];   // faint shadow just above the line
    NSRectFill(NSMakeRect(x0, y0 + 1, W, 1));
    [kFrameBorder() set];                                  // bold #262626 line at the bottom
    NSRectFill(NSMakeRect(x0, y0, W, 1));

    [NSGraphicsContext restoreGraphicsState];
}

// Inactive — FLAT #D6D6D6 fill, no pinstripe, no buttons (per §STATE 5).
// Keeps 1 faint bottom line to separate from content but does NOT emphasize bevel.
+ (void)drawTitleBarInactiveInRect:(NSRect)r {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [kInactiveBar() set];
    NSRectFill(r);
    [[kFrameBorder() colorWithAlphaComponent:0.35] set];   // faint bottom line
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 1));
    [NSGraphicsContext restoreGraphicsState];
}

// §3 — Seamless grip band. r provides x/width + bar height (to vertically center the 13px block).
+ (void)drawTitleGripInRect:(NSRect)r mirrored:(BOOL)mirrored {
    if (r.size.width <= 0) return;
    const CGFloat kH = 13;
    CGFloat gy = floor(r.origin.y + (r.size.height - kH) / 2);
    CGFloat gx = floor(r.origin.x), gw = floor(r.size.width);
    NSRect strip = NSMakeRect(gx, gy, gw, kH);

    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    [kGripBg() set];   NSRectFill(strip);                                  // #DDDDDD fill
    [kGripLine() set];                                                     // #999999 grain / 2px
    for (int i = 1; i <= 11; i += 2)
        NSRectFill(NSMakeRect(gx, gy + i, gw, 1));
    // Lit/dark vertical edges. Default: light left, dark right. Mirrored swaps them so the band's lit
    // "head" faces the opposite way — used for the LEFT band so both bands point inward at the title.
    NSColor *leftEdge = mirrored ? kGripDark() : kGripLight();
    NSColor *rightEdge = mirrored ? kGripLight() : kGripDark();
    [leftEdge set];  NSRectFill(NSMakeRect(gx, gy, 1, kH));                   // left edge
    [rightEdge set]; NSRectFill(NSMakeRect(NSMaxX(strip) - 1, gy, 1, kH));    // right edge

    [NSGraphicsContext restoreGraphicsState];
}

// Title button — drawn directly by PIXEL within r (square corners), non-flipped (larger y = top).
// Big black frame, collapse bars, small zoom square SHARE thickness `t` -> uniform strokes.
// Layers (outer→inner): outer bevel 1px | black frame t px | inner bevel 1px | gradient face.
+ (void)drawTitleButtonInRect:(NSRect)r glyph:(int)glyph pressed:(BOOL)pressed {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    // Pixel-snap: origin + edges to integer pt -> crisp 1px at both 1x & 2x.
    CGFloat x = floor(r.origin.x), y = floor(r.origin.y);
    CGFloat s = floor(MIN(r.size.width, r.size.height));     // square box
    CGFloat R = x + s, T = y + s;                            // right edge / top edge
    NSColor *dark = kFrameBorder();
    CGFloat t = (s >= 15) ? 2 : 1;                           // UNIFORM thickness frame/bar/box

    // (1) Outer bevel 1px (sunken): top+left = #808080, bottom+right = #FFFFFF.
    [kBevelHi() set];                                        // light L (bottom+right) first
    NSRectFill(NSMakeRect(x, y, s, 1));                      // bottom
    NSRectFill(NSMakeRect(R - 1, y, 1, s));                  // right
    [kBevelLo() set];                                        // dark L (top+left) on top
    NSRectFill(NSMakeRect(x, T - 1, s, 1));                  // top
    NSRectFill(NSMakeRect(x, y, 1, s));                      // left

    // (2) Big black frame — t px thick (as bold as the glyph).
    [dark set];
    for (int i = 0; i < (int)t; i++)
        NSFrameRect(NSMakeRect(x + 1 + i, y + 1 + i, s - 2 - 2 * i, s - 2 - 2 * i));

    // Area inside the frame (inner bevel + face). Glyph drawn here -> touches the big frame.
    NSRect area = NSMakeRect(x + 1 + t, y + 1 + t, s - 2 - 2 * t, s - 2 - 2 * t);
    NSRect face = NSInsetRect(area, 1, 1);                   // leave 1px for inner bevel

    // (3) Face: VERTICAL gradient, dark at top (#C9C9C9) -> light at bottom (#F1F1F1).
    NSGradient *fg = [[NSGradient alloc] initWithStartingColor:kFaceTop() endingColor:kFaceBot()];
    [fg drawInRect:face angle:270];

    // (4) Inner bevel 1px (raised): top+left = #FFFFFF, bottom+right = #9A9A9A.
    [kInnerLo() set];                                        // dark L (bottom+right) first
    NSRectFill(NSMakeRect(area.origin.x, area.origin.y, area.size.width, 1));      // bottom
    NSRectFill(NSMakeRect(NSMaxX(area) - 1, area.origin.y, 1, area.size.height));  // right
    [kInnerHi() set];                                        // light L (top+left) on top
    NSRectFill(NSMakeRect(area.origin.x, NSMaxY(area) - 1, area.size.width, 1));   // top
    NSRectFill(NSMakeRect(area.origin.x, area.origin.y, 1, area.size.height));     // left

    // (5) Glyph (#262626) — touches the big frame; stroke thickness t (matches frame).
    if (glyph != 0) {
        [dark set];
        CGFloat ax = area.origin.x, ay = area.origin.y, aw = area.size.width, ah = area.size.height;
        if (glyph == 1) {                        // zoom: small box = 1/4 of big frame (edge at midpoint)
            CGFloat hx = floor(aw / 2), hy = floor(ah / 2);
            // RIGHT edge of small box (vertical): stroke's LEFT side at horizontal midpoint, up to the frame top.
            NSRectFill(NSMakeRect(ax + hx, ay + hy, t, ah - hy));
            // BOTTOM edge of small box (horizontal): stroke's TOP side at vertical midpoint (pulled down) -> 1/4 square box.
            NSRectFill(NSMakeRect(ax, ay + hy - t, hx + t, t));
        } else if (glyph == 2) {                 // collapse: 2 horizontal bars CLOSE together, centered (nudged up)
            CGFloat gap = MAX(1, t - 1);         // small gap between the 2 bars -> tight cluster in the middle
            CGFloat bottomMargin = ceil((ah - (2 * t + gap)) / 2.0);  // surplus pushed to the bottom -> cluster nudges up
            CGFloat b1 = ay + bottomMargin;      // bottom bar
            CGFloat b2 = b1 + t + gap;           // top bar
            NSRectFill(NSMakeRect(ax, b1, aw, t));   // touches both left & right edges of the big frame
            NSRectFill(NSMakeRect(ax, b2, aw, t));
        }
    }

    // (6) Pressed overlay: #353535→#9C9C9C @0.8, diagonal TL→BR, covers FACE only.
    if (pressed) {
        NSGradient *ov = [[NSGradient alloc]
            initWithStartingColor:[kPressTop() colorWithAlphaComponent:0.8]
                      endingColor:[kPressBot() colorWithAlphaComponent:0.8]];
        [ov drawInRect:face angle:315];        // start TL -> end BR
    }

    [NSGraphicsContext restoreGraphicsState];
}

// Dropdown: vertical divider + 2-WAY arrow (▲ top / ▼ bottom) at the right edge (per dropdown.svg).
+ (void)drawDropdownArrowInRect:(NSRect)r {
    CGFloat bw = 16;                          // arrow area on the right
    CGFloat sx = NSMaxX(r) - bw;
    [G(0.5) set];                             // #808080 divider
    NSRectFill(NSMakeRect(sx, r.origin.y + 3, 1, r.size.height - 6));
    [[NSColor whiteColor] set];
    NSRectFill(NSMakeRect(sx + 1, r.origin.y + 3, 1, r.size.height - 6));

    CGFloat cx = sx + bw / 2 + 0.5, cy = NSMidY(r), s = 2.5, gap = 1.5;
    [G(0.15) set];                            // #262626
    // ▲ on top (apex points up — non-flipped: larger y = top)
    NSBezierPath *up = [NSBezierPath bezierPath];
    [up moveToPoint:NSMakePoint(cx, cy + gap + s)];
    [up lineToPoint:NSMakePoint(cx - s, cy + gap)];
    [up lineToPoint:NSMakePoint(cx + s, cy + gap)];
    [up closePath];
    [up fill];
    // ▼ on bottom
    NSBezierPath *dn = [NSBezierPath bezierPath];
    [dn moveToPoint:NSMakePoint(cx, cy - gap - s)];
    [dn lineToPoint:NSMakePoint(cx - s, cy - gap)];
    [dn lineToPoint:NSMakePoint(cx + s, cy - gap)];
    [dn closePath];
    [dn fill];
}

// Platinum horizontally-striped title bar per window.svg (#CCCCCC fill, #DDDDDD band + #999999/2px stripes).
+ (void)drawStripedTitleInRect:(NSRect)r stripesInRect:(NSRect)stripesRect active:(BOOL)active {
    [G(0.8) set]; // #CCCCCC
    NSRectFill(r);
    NSRect band = NSInsetRect(r, 0, 3);
    [G(0.867) set]; // #DDDDDD
    NSRectFill(band);
    // Stripes drawn only in the gap between the 2 icon clusters (not overflowing under icons).
    CGFloat sx = MAX(NSMinX(band), NSMinX(stripesRect));
    CGFloat sw = MIN(NSMaxX(band), NSMaxX(stripesRect)) - sx;
    if (sw > 0) {
        if (active) {
            [G(0.6) set]; // #999999 — horizontal stripe every 2px
            for (CGFloat y = band.origin.y + 1; y < NSMaxY(band); y += 2)
                NSRectFill(NSMakeRect(sx, y, sw, 1));
        }
        // light/dark edges of the band
        [G(0.93) set]; NSRectFill(NSMakeRect(sx, NSMaxY(band) - 1, sw, 1));
        [G(0.77) set]; NSRectFill(NSMakeRect(sx, band.origin.y, sw, 1));
    }
}

// Small serrated corners: 3 steps x1px at each corner (for retro input field).
+ (NSBezierPath *)serratedPathInRect:(NSRect)r {
    CGFloat x0 = r.origin.x, x1 = NSMaxX(r), y0 = r.origin.y, y1 = NSMaxY(r);
    const CGFloat s = 1.0; const int n = 3; const CGFloat c = s * n; // 3px corner
    NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(x0 + c, y1)];
    [p lineToPoint:NSMakePoint(x1 - c, y1)];                 // top edge
    for (int i = 0; i < n; i++) {                            // serrated top-right corner
        [p lineToPoint:NSMakePoint(x1 - c + (i + 1) * s, y1 - i * s)];
        [p lineToPoint:NSMakePoint(x1 - c + (i + 1) * s, y1 - (i + 1) * s)];
    }
    [p lineToPoint:NSMakePoint(x1, y0 + c)];                 // right edge
    for (int i = 0; i < n; i++) {                            // bottom-right corner
        [p lineToPoint:NSMakePoint(x1 - i * s, y0 + c - (i + 1) * s)];
        [p lineToPoint:NSMakePoint(x1 - (i + 1) * s, y0 + c - (i + 1) * s)];
    }
    [p lineToPoint:NSMakePoint(x0 + c, y0)];                 // bottom edge
    for (int i = 0; i < n; i++) {                            // bottom-left corner
        [p lineToPoint:NSMakePoint(x0 + c - (i + 1) * s, y0 + i * s)];
        [p lineToPoint:NSMakePoint(x0 + c - (i + 1) * s, y0 + (i + 1) * s)];
    }
    [p lineToPoint:NSMakePoint(x0, y1 - c)];                 // left edge
    for (int i = 0; i < n; i++) {                            // top-left corner
        [p lineToPoint:NSMakePoint(x0 + i * s, y1 - c + (i + 1) * s)];
        [p lineToPoint:NSMakePoint(x0 + (i + 1) * s, y1 - c + (i + 1) * s)];
    }
    [p closePath];
    return p;
}

+ (void)drawCheckInRect:(NSRect)r color:(NSColor *)c {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];   // pixel-crisp edges (Platinum)
    NSBezierPath *p = [NSBezierPath bezierPath];
    p.lineWidth = 2.0;
    p.lineCapStyle = NSLineCapStyleSquare;
    p.lineJoinStyle = NSLineJoinStyleMiter;
    CGFloat x = NSMinX(r), y = NSMinY(r), w = r.size.width, h = r.size.height;
    // Flipped coords (y grows downward): short arm DOWN to the bottom vertex, then long arm UP to the right.
    [p moveToPoint:NSMakePoint(x + w * 0.16, y + h * 0.52)];
    [p lineToPoint:NSMakePoint(x + w * 0.40, y + h * 0.76)];
    [p lineToPoint:NSMakePoint(x + w * 0.86, y + h * 0.22)];
    [c set];
    [p stroke];
    [NSGraphicsContext restoreGraphicsState];
}

@end
