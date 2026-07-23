#import "theme/OS9Theme.h"

static NSColor *G(CGFloat v) { return [NSColor colorWithCalibratedWhite:v alpha:1.0]; }
static NSColor *GA(CGFloat v, CGFloat a) { return [NSColor colorWithCalibratedWhite:v alpha:a]; }
static NSColor *RGB(CGFloat r, CGFloat g, CGFloat b) {
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:1.0];
}

@implementation OS9Theme

// === Palette engine =============================================================================
// Color tokens are CONSTANT per theme -> built ONCE (avoids allocating NSColor on every drawRect).
// setThemeName: is called a single time at startup (before widgets are built) and resets the cache,
// so a token touched before the theme is set still ends up correct after the reset.
// Getters run only on the main thread (UI drawing), so the lazy build is safe.
static BOOL gDark = NO;
static BOOL gPaletteBuilt = NO;

// public surface tokens
static NSColor *gFace, *gButtonFace, *gFaceLight, *gHighlight, *gShadow, *gDarkShadow, *gFrame,
    *gWindowBg, *gAccent, *gRowSelectionGray, *gTitleActive;
// semantic tokens
static NSColor *gTextPrimary, *gTextSecondary, *gInsetBg, *gTitleTextActive, *gTitleTextInactive,
    *gMenuHoverBg, *gMenuHoverText, *gMenuSeparator, *gMenuBorder, *gStatusOk, *gStatusError,
    *gToastBg, *gToastOk, *gToastError, *gToastInfo,
    *gScrollerThumb, *gScrollerBorder, *gScrollerGripDark, *gScrollerGripLight,
    *gEditorBg, *gEditorFg, *gEditorString, *gEditorNumber, *gEditorProperty, *gEditorSelectionBg,
    *gEditorBraceFg, *gEditorBraceBg, *gEditorBraceBadFg,
    *gGlyphStroke, *gGlyphBoxFill, *gGlyphBoxHighlight, *gGlyphBoxOutline;
// title bar palette (PROMPT_os9_titlebar_objcpp.md §"COLOR PALETTE"; dark variant per dark.jpg)
static NSColor *gTbFrameBorder, *gTbGlyph, *gTbBarFill, *gTbInactiveBar, *gTbGripBg, *gTbGripLight,
    *gTbGripDark, *gTbGripLine, *gTbBevelHi, *gTbBevelLo, *gTbFaceTop, *gTbFaceBot, *gTbInnerHi,
    *gTbInnerLo, *gTbPressTop, *gTbPressBot,
    // close/zoom/collapse button edges only (dark theme wants BLACK edges, while the generic
    // bevel/control-box keeps its light-vs-dark bevel pair)
    *gTbBtnEdgeHi, *gTbBtnEdgeLo, *gTbBtnInnerHi, *gTbBtnInnerLo;
// button internals (line/new styles draw their own fills/borders)
static NSColor *gLineBtnFill, *gLineBtnFillPressed, *gLineBtnBorder, *gLineBtnPressedFG,
    *gNewBtnFill, *gNewBtnFillPressed, *gNewBtnBevel, *gNewBtnBorder, *gClassicBtnFillPressed;

static void ensurePalette(void) {
    if (gPaletteBuilt) return;
    gPaletteBuilt = YES;
    if (!gDark) {
        // --- Light Platinum (original) ---
        gFace = G(0.80); gButtonFace = G(0.867); gFaceLight = G(0.88);          // #CCC / #DDD
        gHighlight = G(1.00); gShadow = G(0.53); gDarkShadow = G(0.33);
        gFrame = G(0.0); gWindowBg = G(0.80); gTitleActive = G(0.80);
        gAccent = RGB(0.20, 0.30, 0.55); gRowSelectionGray = G(0.82);
        gTextPrimary = [NSColor blackColor]; gTextSecondary = G(0.4); gInsetBg = [NSColor whiteColor];
        gTitleTextActive = G(0.149); gTitleTextInactive = G(0.541);             // #262626 / #8A8A8A
        gMenuHoverBg = RGB(0.2, 0.2, 0.6); gMenuHoverText = [NSColor whiteColor];
        gMenuSeparator = G(0.5); gMenuBorder = G(0.15);
        gStatusOk = RGB(0.0, 0.45, 0.0); gStatusError = RGB(0.6, 0.0, 0.0);
        gToastBg = G(0.82);
        gToastOk = RGB(0.29, 0.59, 0.40); gToastError = RGB(0.78, 0.25, 0.22);
        gToastInfo = RGB(0.42, 0.50, 0.69);
        gScrollerThumb = GA(0.6, 0.95); gScrollerBorder = GA(0.15, 0.95);       // #999 / #262626
        gScrollerGripDark = GA(0.15, 0.55); gScrollerGripLight = GA(1.0, 0.6);
        gEditorBg = [NSColor whiteColor]; gEditorFg = [NSColor blackColor];
        gEditorString = RGB(0.0, 0.45, 0.0); gEditorNumber = RGB(0.1, 0.2, 0.8);
        gEditorProperty = RGB(0.45, 0.1, 0.5); gEditorSelectionBg = RGB(0.78, 0.82, 0.95);
        gEditorBraceFg = RGB(0.0, 0.0, 0.55); gEditorBraceBg = RGB(1.0, 0.92, 0.55);
        gEditorBraceBadFg = RGB(0.8, 0.0, 0.0);
        gGlyphStroke = [NSColor blackColor];
        gGlyphBoxFill = RGB(0.62, 0.74, 0.86); gGlyphBoxHighlight = RGB(0.82, 0.90, 0.97);
        gGlyphBoxOutline = RGB(0.27, 0.38, 0.50);
        gTbFrameBorder = G(0.149); gTbGlyph = G(0.149);                          // #262626
        gTbBarFill = G(0.800); gTbInactiveBar = G(0.839);                        // #CCC / #D6D6D6
        gTbGripBg = G(0.867); gTbGripLight = G(0.933); gTbGripDark = G(0.773);   // #DDD / #EEE / #C5C5C5
        gTbGripLine = G(0.600);                                                  // #999999
        gTbBevelHi = [NSColor whiteColor]; gTbBevelLo = G(0.502);                // #FFF / #808080
        gTbFaceTop = G(0.788); gTbFaceBot = G(0.945);                            // #C9C9C9 / #F1F1F1
        gTbInnerHi = [NSColor whiteColor]; gTbInnerLo = G(0.604);                // #FFF / #9A9A9A
        gTbBtnEdgeHi = [NSColor whiteColor]; gTbBtnEdgeLo = G(0.502);            // = bevel pair (light)
        gTbBtnInnerHi = [NSColor whiteColor]; gTbBtnInnerLo = G(0.604);
        gTbPressTop = G(0.208); gTbPressBot = G(0.612);                          // #353535 / #9C9C9C
        gLineBtnFill = [NSColor whiteColor]; gLineBtnFillPressed = G(0.20);
        gLineBtnBorder = G(0.15); gLineBtnPressedFG = [NSColor whiteColor];      // inverted dark fill
        gNewBtnFill = G(0.80); gNewBtnFillPressed = G(0.70);                     // #CCCCCC
        gNewBtnBevel = G(0.5); gNewBtnBorder = G(0.282);                         // #808080 / #484848
        gClassicBtnFillPressed = G(0.62);
    } else {
        // --- Dark Platinum (per assets/images/dark.jpg). Bevel keeps TL-light/BR-dark orientation,
        // only the value range is compressed so raised/sunken still reads on dark surfaces. ---
        gFace = G(0.18); gButtonFace = G(0.227); gFaceLight = G(0.24);           // #2E2E2E / #3A3A3A
        gHighlight = G(0.43); gShadow = G(0.08); gDarkShadow = G(0.03);          // #6E6E6E / #141414
        gFrame = G(0.0); gWindowBg = G(0.165); gTitleActive = G(0.227);          // #2A2A2A
        gAccent = RGB(0.50, 0.50, 0.78); gRowSelectionGray = RGB(0.23, 0.23, 0.31); // #8080C7 / #3B3B4F
        gTextPrimary = G(0.90); gTextSecondary = G(0.60); gInsetBg = G(0.102);   // #E6E6E6 / #1A1A1A
        gTitleTextActive = G(0.90); gTitleTextInactive = G(0.55);
        gMenuHoverBg = RGB(0.30, 0.30, 0.55); gMenuHoverText = [NSColor whiteColor];
        gMenuSeparator = G(0.45); gMenuBorder = G(0.05);
        gStatusOk = RGB(0.35, 0.80, 0.42); gStatusError = RGB(0.90, 0.42, 0.42);
        gToastBg = G(0.23);
        gToastOk = RGB(0.24, 0.42, 0.28); gToastError = RGB(0.48, 0.22, 0.22);
        gToastInfo = RGB(0.26, 0.26, 0.36);
        gScrollerThumb = GA(0.29, 0.95); gScrollerBorder = GA(0.05, 0.95);       // #4A4A4A / #0D0D0D
        gScrollerGripDark = GA(0.0, 0.55); gScrollerGripLight = GA(0.42, 0.6);   // #6A6A6A
        gEditorBg = G(0.102); gEditorFg = G(0.863);                              // #1A1A1A / #DCDCDC
        gEditorString = RGB(0.50, 0.78, 0.50); gEditorNumber = RGB(0.50, 0.66, 0.91);
        gEditorProperty = RGB(0.78, 0.62, 0.88); gEditorSelectionBg = RGB(0.22, 0.22, 0.36);
        gEditorBraceFg = RGB(0.62, 0.78, 1.0); gEditorBraceBg = RGB(0.27, 0.27, 0.16);
        gEditorBraceBadFg = RGB(1.0, 0.42, 0.42);
        gGlyphStroke = G(0.90);
        gGlyphBoxFill = RGB(0.55, 0.55, 0.80); gGlyphBoxHighlight = G(0.43);
        gGlyphBoxOutline = G(0.05);
        gTbFrameBorder = G(0.05); gTbGlyph = G(0.867);                           // #0D0D0D / #DDDDDD
        gTbBarFill = G(0.20); gTbInactiveBar = G(0.17);                          // #333333 / #2B2B2B
        gTbGripBg = G(0.227); gTbGripLight = G(0.31); gTbGripDark = G(0.15);
        gTbGripLine = [NSColor whiteColor];                                      // white pinstripe on dark band
        gTbBevelHi = G(0.43); gTbBevelLo = G(0.05);
        gTbFaceTop = G(0.275); gTbFaceBot = G(0.353);                            // #464646 / #5A5A5A
        gTbInnerHi = G(0.46); gTbInnerLo = G(0.12);
        gTbBtnEdgeHi = G(0.06); gTbBtnEdgeLo = G(0.03);                          // BLACK edges on the
        gTbBtnInnerHi = G(0.10); gTbBtnInnerLo = G(0.05);                        // close/zoom/collapse boxes
        gTbPressTop = G(0.04); gTbPressBot = G(0.235);
        gLineBtnFill = G(0.227); gLineBtnFillPressed = G(0.75);                  // pressed inverts LIGHT
        gLineBtnBorder = G(0.05); gLineBtnPressedFG = [NSColor blackColor];      // dark text on light fill
        gNewBtnFill = G(0.227); gNewBtnFillPressed = G(0.16);
        gNewBtnBevel = G(0.06); gNewBtnBorder = G(0.05);
        gClassicBtnFillPressed = G(0.14);
    }
}
// Token getter: every public color routes through the cached palette.
#define TOKEN(name, var) + (NSColor *)name { ensurePalette(); return var; }

+ (void)setThemeName:(NSString *)name {
    gDark = [name isEqualToString:@"dark"];
    gPaletteBuilt = NO;   // rebuild on next token access (protects accesses made before theme is set)
}
+ (BOOL)isDarkTheme { return gDark; }

TOKEN(face, gFace)
TOKEN(buttonFace, gButtonFace)
TOKEN(faceLight, gFaceLight)
TOKEN(highlight, gHighlight)
TOKEN(shadow, gShadow)
TOKEN(darkShadow, gDarkShadow)
TOKEN(frame, gFrame)
TOKEN(windowBg, gWindowBg)
TOKEN(accent, gAccent)
TOKEN(rowSelectionGray, gRowSelectionGray)
TOKEN(titleActive, gTitleActive)
TOKEN(textPrimary, gTextPrimary)
TOKEN(textSecondary, gTextSecondary)
TOKEN(insetBg, gInsetBg)
TOKEN(titleTextActive, gTitleTextActive)
TOKEN(titleTextInactive, gTitleTextInactive)
TOKEN(menuHoverBg, gMenuHoverBg)
TOKEN(menuHoverText, gMenuHoverText)
TOKEN(menuSeparator, gMenuSeparator)
TOKEN(menuBorder, gMenuBorder)
TOKEN(statusOk, gStatusOk)
TOKEN(statusError, gStatusError)
TOKEN(toastBg, gToastBg)
TOKEN(toastOk, gToastOk)
TOKEN(toastError, gToastError)
TOKEN(toastInfo, gToastInfo)
TOKEN(scrollerThumb, gScrollerThumb)
TOKEN(scrollerBorder, gScrollerBorder)
TOKEN(scrollerGripDark, gScrollerGripDark)
TOKEN(scrollerGripLight, gScrollerGripLight)
TOKEN(editorBg, gEditorBg)
TOKEN(editorFg, gEditorFg)
TOKEN(editorString, gEditorString)
TOKEN(editorNumber, gEditorNumber)
TOKEN(editorProperty, gEditorProperty)
TOKEN(editorSelectionBg, gEditorSelectionBg)
TOKEN(editorBraceFg, gEditorBraceFg)
TOKEN(editorBraceBg, gEditorBraceBg)
TOKEN(editorBraceBadFg, gEditorBraceBadFg)
TOKEN(glyphStroke, gGlyphStroke)
TOKEN(glyphBoxFill, gGlyphBoxFill)
TOKEN(glyphBoxHighlight, gGlyphBoxHighlight)
TOKEN(glyphBoxOutline, gGlyphBoxOutline)
#undef TOKEN

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
    ensurePalette();
    if (pressed && gButtonStyle == 0) return gLineBtnPressedFG; // line: inverted fill -> opposite ink
    return enabled ? gTextPrimary : [self shadow];
}

// Retro border-line: flat white/light fill + bold stroked border, SQUARE corners. Pressed -> inverted fill.
+ (void)drawLineButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    ensurePalette();
    // Fill the WHOLE bounds (no inset) to fully clear edges on each draw -> avoids antialiased border
    // accumulating alpha on redraw (view is not layer-backed, draws source-over into shared backing).
    [(pressed ? gLineBtnFillPressed : gLineBtnFill) set];
    NSRectFill(r);
    [gLineBtnBorder set];                                // #262626 border
    NSFrameRectWithWidth(r, isDefault ? 2.0 : 1.0);      // crisp border (no AA), idempotent
}

// btn-new.svg: #CCCCCC fill, square corners, #484848 1px border, white (top-left) / #808080 (bottom-right) bevel.
+ (void)drawNewBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    ensurePalette();
    [(pressed ? gNewBtnFillPressed : gNewBtnFill) set]; // #CCCCCC
    NSRectFill(r);                       // fill full bounds (see drawLineButtonInRect note)

    NSRect bz = NSInsetRect(r, 1, 1);    // bevel just inside the 1px border
    NSColor *tl = pressed ? gNewBtnBevel : gTbBevelHi;
    NSColor *br = pressed ? gTbBevelHi : gNewBtnBevel;   // #808080
    CGFloat x0 = bz.origin.x, x1 = NSMaxX(bz), y0 = bz.origin.y, y1 = NSMaxY(bz);
    [tl set];
    NSRectFill(NSMakeRect(x0, y1 - 2, bz.size.width, 2));   // top (non-flipped: maxY = top)
    NSRectFill(NSMakeRect(x0, y0, 2, bz.size.height));       // left
    [br set];
    NSRectFill(NSMakeRect(x0, y0, bz.size.width, 2));        // bottom
    NSRectFill(NSMakeRect(x1 - 2, y0, 2, bz.size.height));   // right

    [gNewBtnBorder set]; // #484848 border
    NSFrameRectWithWidth(r, isDefault ? 2.0 : 1.0);          // crisp border (no AA), idempotent
}

+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault {
    ensurePalette();
    NSBezierPath *path = [self steppedPathInRect:NSInsetRect(r, 0.5, 0.5)];

    // #DDDDDD fill (sunken when pressed)
    [(pressed ? gClassicBtnFillPressed : gButtonFace) set];
    [path fill];

    // Disable antialias for border strokes: stroke covers WHOLE pixels (alpha 1) so redraw doesn't
    // accumulate -> avoids "border darkening" on rapid clicks (view not layer-backed, draws source-over).
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    // Inner bevel: drawn along the SERRATED PATH (inset) so corners — even top-left — are serrated too.
    // highlight top-left, shadow bottom-right; each half clipped by a diagonal triangle.
    NSColor *tl = pressed ? gShadow : gHighlight;
    NSColor *br = pressed ? gHighlight : gShadow;
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
    ensurePalette();
    NSRect inner = NSInsetRect(r, 0.5, 0.5);
    [gInsetBg set];
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
    ensurePalette();
    // Outer sunken frame: dark (#808080) top-left, light (white) bottom-right.
    [gTbBevelLo set];
    NSRectFill(NSMakeRect(r.origin.x, NSMaxY(r) - 1, r.size.width, 1));  // top
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, 1, r.size.height));    // left
    [gTbBevelHi set];
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 1));      // bottom
    NSRectFill(NSMakeRect(NSMaxX(r) - 1, r.origin.y, 1, r.size.height));  // right

    // Inner box: gradient #9A9A9A -> #F1F1F1 (top-left -> bottom-right).
    NSRect ib = NSInsetRect(r, 2, 2);
    NSGradient *grad = [[NSGradient alloc] initWithStartingColor:gTbInnerLo endingColor:gTbFaceBot];
    [grad drawInRect:ib angle:-45];

    // inner box bevel: white top-left, gray bottom-right
    [gTbInnerHi set];
    NSRectFill(NSMakeRect(ib.origin.x, NSMaxY(ib) - 1, ib.size.width, 1));
    NSRectFill(NSMakeRect(ib.origin.x, ib.origin.y, 1, ib.size.height));
    [gTbInnerLo set];
    NSRectFill(NSMakeRect(ib.origin.x, ib.origin.y, ib.size.width, 1));
    NSRectFill(NSMakeRect(NSMaxX(ib) - 1, ib.origin.y, 1, ib.size.height));

    // box border #262626 (glyph uses the theme glyph ink so it stays visible on the dark face)
    NSColor *dark = gTbGlyph;
    [gTbFrameBorder set];
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
// Exact hex palette per the prompt's §"COLOR PALETTE" (light values); dark variant in ensurePalette.
static NSColor *kFrameBorder(void) { ensurePalette(); return gTbFrameBorder; } // #262626 — border
static NSColor *kGlyph(void)       { ensurePalette(); return gTbGlyph; }       // glyph/ink on button face
static NSColor *kBarFill(void)     { ensurePalette(); return gTbBarFill; }     // #CCCCCC — active bar bg
static NSColor *kInactiveBar(void) { ensurePalette(); return gTbInactiveBar; } // #D6D6D6 — inactive bar bg
static NSColor *kGripBg(void)      { ensurePalette(); return gTbGripBg; }      // #DDDDDD — pinstripe bg
static NSColor *kGripLight(void)   { ensurePalette(); return gTbGripLight; }   // #EEEEEE — light left edge
static NSColor *kGripDark(void)    { ensurePalette(); return gTbGripDark; }    // #C5C5C5 — dark right edge
static NSColor *kGripLine(void)    { ensurePalette(); return gTbGripLine; }    // #999999 — line 1px/2px
static NSColor *kBevelHi(void)     { ensurePalette(); return gTbBevelHi; }     // #FFFFFF
static NSColor *kBevelLo(void)     { ensurePalette(); return gTbBevelLo; }     // #808080 — dark outer bevel
static NSColor *kFaceTop(void)     { ensurePalette(); return gTbFaceTop; }     // #C9C9C9 — face top (dark)
static NSColor *kFaceBot(void)     { ensurePalette(); return gTbFaceBot; }     // #F1F1F1 — face bottom
// (generic inner-bevel pair gTbInnerHi/gTbInnerLo is used directly by drawMacControlBox)
// close/zoom/collapse box edges (light: same pair as bevel; dark: all near-black)
static NSColor *kBtnEdgeHi(void)   { ensurePalette(); return gTbBtnEdgeHi; }
static NSColor *kBtnEdgeLo(void)   { ensurePalette(); return gTbBtnEdgeLo; }
static NSColor *kBtnInnerHi(void)  { ensurePalette(); return gTbBtnInnerHi; }
static NSColor *kBtnInnerLo(void)  { ensurePalette(); return gTbBtnInnerLo; }
static NSColor *kPressTop(void)    { ensurePalette(); return gTbPressTop; }    // #353535 — pressed (TL)
static NSColor *kPressBot(void)    { ensurePalette(); return gTbPressBot; }    // #9C9C9C — pressed (BR)

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
    NSColor *frameInk = kFrameBorder();
    CGFloat t = (s >= 15) ? 2 : 1;                           // UNIFORM thickness frame/bar/box

    // (1) Outer bevel 1px (sunken): top+left = #808080, bottom+right = #FFFFFF.
    [kBtnEdgeHi() set];                                      // light L (bottom+right) first
    NSRectFill(NSMakeRect(x, y, s, 1));                      // bottom
    NSRectFill(NSMakeRect(R - 1, y, 1, s));                  // right
    [kBtnEdgeLo() set];                                      // dark L (top+left) on top
    NSRectFill(NSMakeRect(x, T - 1, s, 1));                  // top
    NSRectFill(NSMakeRect(x, y, 1, s));                      // left

    // (2) Big black frame — t px thick (as bold as the glyph).
    [frameInk set];
    for (int i = 0; i < (int)t; i++)
        NSFrameRect(NSMakeRect(x + 1 + i, y + 1 + i, s - 2 - 2 * i, s - 2 - 2 * i));

    // Area inside the frame (inner bevel + face). Glyph drawn here -> touches the big frame.
    NSRect area = NSMakeRect(x + 1 + t, y + 1 + t, s - 2 - 2 * t, s - 2 - 2 * t);
    NSRect face = NSInsetRect(area, 1, 1);                   // leave 1px for inner bevel

    // (3) Face: VERTICAL gradient, dark at top (#C9C9C9) -> light at bottom (#F1F1F1).
    NSGradient *fg = [[NSGradient alloc] initWithStartingColor:kFaceTop() endingColor:kFaceBot()];
    [fg drawInRect:face angle:270];

    // (4) Inner bevel 1px (raised): top+left = #FFFFFF, bottom+right = #9A9A9A.
    [kBtnInnerLo() set];                                     // dark L (bottom+right) first
    NSRectFill(NSMakeRect(area.origin.x, area.origin.y, area.size.width, 1));      // bottom
    NSRectFill(NSMakeRect(NSMaxX(area) - 1, area.origin.y, 1, area.size.height));  // right
    [kBtnInnerHi() set];                                     // light L (top+left) on top
    NSRectFill(NSMakeRect(area.origin.x, NSMaxY(area) - 1, area.size.width, 1));   // top
    NSRectFill(NSMakeRect(area.origin.x, area.origin.y, 1, area.size.height));     // left

    // (5) Glyph — touches the big frame; stroke thickness t (matches frame). kGlyph so the
    // ink stays visible on the dark-theme face (frame border stays dark, glyph goes light).
    if (glyph != 0) {
        [kGlyph() set];
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
    [kBevelLo() set];                         // #808080 divider
    NSRectFill(NSMakeRect(sx, r.origin.y + 3, 1, r.size.height - 6));
    [kBevelHi() set];
    NSRectFill(NSMakeRect(sx + 1, r.origin.y + 3, 1, r.size.height - 6));

    CGFloat cx = sx + bw / 2 + 0.5, cy = NSMidY(r), s = 2.5, gap = 1.5;
    [kGlyph() set];                           // #262626
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
    [kBarFill() set]; // #CCCCCC
    NSRectFill(r);
    NSRect band = NSInsetRect(r, 0, 3);
    [kGripBg() set]; // #DDDDDD
    NSRectFill(band);
    // Stripes drawn only in the gap between the 2 icon clusters (not overflowing under icons).
    CGFloat sx = MAX(NSMinX(band), NSMinX(stripesRect));
    CGFloat sw = MIN(NSMaxX(band), NSMaxX(stripesRect)) - sx;
    if (sw > 0) {
        if (active) {
            [kGripLine() set]; // #999999 — horizontal stripe every 2px
            for (CGFloat y = band.origin.y + 1; y < NSMaxY(band); y += 2)
                NSRectFill(NSMakeRect(sx, y, sw, 1));
        }
        // light/dark edges of the band
        [kGripLight() set]; NSRectFill(NSMakeRect(sx, NSMaxY(band) - 1, sw, 1));
        [kGripDark() set];  NSRectFill(NSMakeRect(sx, band.origin.y, sw, 1));
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
