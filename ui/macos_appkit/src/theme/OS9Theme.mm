#import "theme/OS9Theme.h"
#import "theme/OS9ThemeInternal.h"

// Non-static: shared with OS9ThemeTitleBar.mm (declared in OS9ThemeInternal.h).
NSColor *G(CGFloat v) { return [NSColor colorWithCalibratedWhite:v alpha:1.0]; }

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

@end
