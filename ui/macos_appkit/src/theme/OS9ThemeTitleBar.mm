#import "theme/OS9Theme.h"
#import "theme/OS9ThemeInternal.h"

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

@implementation OS9Theme (TitleBar)

// Bar frame: #CCCCCC fill + bold #262626 bottom line. Idempotent draw (opaque fill first ->
// alpha doesn't accumulate across drawRects).
+ (void)drawTitleBarFrameInRect:(NSRect)r {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];

    CGFloat W = r.size.width;
    CGFloat x0 = r.origin.x, y0 = r.origin.y;

    // Body fill #CCCCCC (covers all -> resets backing for the bar area).
    [kBarFill() set];
    NSRectFill(r);

    // Only 1 BOLD line + shadow at the BOTTOM edge of the title bar (y0 = bottom, non-flipped).
    [[kFrameBorder() colorWithAlphaComponent:0.25] set];   // faint shadow just above the line
    NSRectFill(NSMakeRect(x0, y0 + 1, W, 1));
    [kFrameBorder() set];                                  // bold #262626 line at the bottom
    NSRectFill(NSMakeRect(x0, y0, W, 1));

    [NSGraphicsContext restoreGraphicsState];
}

// Inactive — FLAT #D6D6D6 fill, no pinstripe, no buttons; 1 faint bottom line separates from content.
+ (void)drawTitleBarInactiveInRect:(NSRect)r {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [kInactiveBar() set];
    NSRectFill(r);
    [[kFrameBorder() colorWithAlphaComponent:0.35] set];   // faint bottom line
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 1));
    [NSGraphicsContext restoreGraphicsState];
}

// Seamless grip band. r provides x/width + bar height (to vertically center the 13px block).
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
