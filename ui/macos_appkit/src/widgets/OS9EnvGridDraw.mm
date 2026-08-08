#import "widgets/OS9EnvGridInternal.h"
#import "app/AppStrings.h"
#import "theme/OS9Theme.h"

void DrawClose(NSRect box, NSColor *c) {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    NSBezierPath *p = [NSBezierPath bezierPath];
    p.lineWidth = 1.5;
    CGFloat in = 3;
    [p moveToPoint:NSMakePoint(NSMinX(box) + in, NSMinY(box) + in)];
    [p lineToPoint:NSMakePoint(NSMaxX(box) - in, NSMaxY(box) - in)];
    [p moveToPoint:NSMakePoint(NSMaxX(box) - in, NSMinY(box) + in)];
    [p lineToPoint:NSMakePoint(NSMinX(box) + in, NSMaxY(box) - in)];
    [c set];
    [p stroke];
    [NSGraphicsContext restoreGraphicsState];
}
void DrawPlus(NSRect box, NSColor *c) {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    NSBezierPath *p = [NSBezierPath bezierPath];
    p.lineWidth = 1.5;
    CGFloat cx = NSMidX(box), cy = NSMidY(box), in = 3;
    [p moveToPoint:NSMakePoint(NSMinX(box) + in, cy)];
    [p lineToPoint:NSMakePoint(NSMaxX(box) - in, cy)];
    [p moveToPoint:NSMakePoint(cx, NSMinY(box) + in)];
    [p lineToPoint:NSMakePoint(cx, NSMaxY(box) - in)];
    [c set];
    [p stroke];
    [NSGraphicsContext restoreGraphicsState];
}

NSDictionary *TextAttrs(NSColor *fg) {
    return @{ NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : fg };
}

// Truncate string to fit maxW, appending "…" (drawInRect does NOT truncate -> do it ourselves).
NSString *Ellipsize(NSString *s, CGFloat maxW, NSDictionary *attrs) {
    if (maxW <= 0) return @"";
    if ([s sizeWithAttributes:attrs].width <= maxW) return s;
    NSString *e = @"…";
    NSUInteger lo = 0, hi = s.length;          // find the largest char count where "<prefix>…" still fits
    while (hi > lo) {
        NSUInteger mid = (lo + hi + 1) / 2;
        NSString *cand = [[s substringToIndex:mid] stringByAppendingString:e];
        if ([cand sizeWithAttributes:attrs].width <= maxW) lo = mid; else hi = mid - 1;
    }
    return [[s substringToIndex:lo] stringByAppendingString:e];
}

// Draw 1 line of text in a cell: CLIP + ellipsize + vertically centered.
void DrawCellText(NSString *s, NSRect cell, NSColor *fg) {
    if (!s.length) return;
    NSDictionary *attrs = TextAttrs(fg);
    NSRect in = NSInsetRect(cell, 6, 4);
    NSString *show = Ellipsize(s, in.size.width, attrs);
    NSSize sz = [show sizeWithAttributes:attrs];
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(cell);
    [show drawAtPoint:NSMakePoint(in.origin.x, in.origin.y + floor((in.size.height - sz.height) / 2))
       withAttributes:attrs];
    [NSGraphicsContext restoreGraphicsState];
}

@implementation OS9EnvGrid (Draw)

#pragma mark drawing helpers

- (void)drawVDivAt:(CGFloat)x height:(CGFloat)h {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [[OS9Theme shadow] set];
    NSRectFill(NSMakeRect(x, 0, 1, h));
    [NSGraphicsContext restoreGraphicsState];
}
- (void)drawHDivAt:(CGFloat)y width:(CGFloat)w {
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [[OS9Theme shadow] set];
    NSRectFill(NSMakeRect(0, y - 1, w, 1));
    [NSGraphicsContext restoreGraphicsState];
}
- (NSRect)closeBoxInRect:(NSRect)r {   // × at right corner of env header
    return NSMakeRect(NSMaxX(r) - kGlyph - 4, (kHeaderH - kGlyph) / 2, kGlyph, kGlyph);
}
- (NSRect)closeBoxInAliasRowAtY:(CGFloat)y {
    return NSMakeRect(_aliasW - kGlyph - 5, y + (kRowH - kGlyph) / 2, kGlyph, kGlyph);
}

#pragma mark header drawing

- (void)drawHeaderIn:(NSView *)v {
    NSRect b = v.bounds;
    [[OS9Theme buttonFace] set];
    NSRectFill(b);

    CGFloat dx = -[self scrollX];   // header scrolls horizontally in sync with body; sticky VERTICALLY.
    NSColor *fg = [OS9Theme frame];

    DrawCellText(StrGridAlias, NSMakeRect(dx, 0, _aliasW, kHeaderH), fg);
    [self drawVDivAt:_aliasW + dx height:kHeaderH];

    for (NSInteger e = 0; e < (NSInteger)_envNames.count; e++) {
        NSRect r = [self envRectAtIndex:e height:kHeaderH];
        r.origin.x += dx;
        [self drawVDivAt:NSMaxX(r) height:kHeaderH];
        BOOL showX = (e == _hoverEnvCol) && !(_protectedFirstColumn && e == 0);   // no × on protected col
        NSRect txt = r;
        if (showX) txt.size.width -= (kGlyph + 6);     // leave room for × on hover
        DrawCellText([self displayForEnv:e], txt, fg);
        if (showX) DrawClose([self closeBoxInRect:r], [OS9Theme shadow]);
    }
    // Trailing column: "+" add-env in the header, the per-row secret toggles below it (body).
    CGFloat addX = [self trailingX] + dx;
    CGFloat trailW = [self trailingColW];
    [self drawVDivAt:addX height:kHeaderH];
    DrawPlus(NSInsetRect(NSMakeRect(addX, 0, trailW, kHeaderH), (trailW - kGlyph) / 2, 4), [OS9Theme frame]);

    [self drawHDivAt:kHeaderH width:b.size.width];
}

#pragma mark body drawing

- (void)drawBodyIn:(NSView *)v dirty:(NSRect)dirty {
    [[NSColor whiteColor] set];
    NSRectFill(v.bounds);

    NSInteger nRows = _aliases.count;
    NSInteger nCols = _envNames.count;
    CGFloat cw = [self contentWidth];

    // Zebra/selection background + grid: NSRectFill is cheap, let AppKit clip to dirty (draws outside dirty are dropped).
    for (NSInteger row = 0; row < nRows; row++) {
        NSRect rr = NSMakeRect(0, row * kRowH, cw, kRowH);
        if (row == _selectedRow) { [[OS9Theme accent] set]; NSRectFill(rr); }
        else if (row % 2 == 1) { [[OS9Theme rowSelectionGray] set]; NSRectFill(rr); }
    }
    [[OS9Theme face] set];
    NSRectFill(NSMakeRect(0, nRows * kRowH, cw, kAddRowH));

    // vertical grid (alias edge + each env edge; the last env edge is also the trailing column's left edge).
    [self drawVDivAt:_aliasW height:[self bodyHeight]];
    for (NSInteger e = 0; e < nCols; e++)
        [self drawVDivAt:[self envContentX:e] + [self envWidth:e] height:[self bodyHeight]];
    // horizontal grid.
    for (NSInteger row = 1; row <= nRows; row++) {
        [NSGraphicsContext saveGraphicsState];
        [[NSGraphicsContext currentContext] setShouldAntialias:NO];
        [[OS9Theme rowSelectionGray] set];
        NSRectFill(NSMakeRect(0, row * kRowH, cw, 1));
        [NSGraphicsContext restoreGraphicsState];
    }

    // Text + per-cell delegate query is the EXPENSIVE part (Ellipsize binary-search + valueForAlias).
    // Run only for rows INTERSECTING the dirty rect -> hover/selection (1-row invalidate) doesn't re-query
    // all rows×cols. Full redraw (layout/reload) -> dirty = bounds -> all rows still drawn.
    NSInteger firstRow = MAX((NSInteger)0, (NSInteger)floor(NSMinY(dirty) / kRowH));
    NSInteger lastRow  = MIN(nRows - 1, (NSInteger)floor((NSMaxY(dirty) - 1) / kRowH));
    for (NSInteger row = firstRow; row <= lastRow; row++) {
        BOOL sel = (row == _selectedRow);
        NSColor *fg = sel ? [NSColor whiteColor] : [OS9Theme frame];
        CGFloat y = row * kRowH;
        NSRect aliasCell = NSMakeRect(0, y, _aliasW, kRowH);
        if (row == _hoverRow || sel) aliasCell.size.width -= (kGlyph + 6);  // leave room for × delete alias
        DrawCellText(_aliases[row], aliasCell, fg);
        if (row == _hoverRow || sel)
            DrawClose([self closeBoxInAliasRowAtY:y], sel ? [NSColor whiteColor] : [OS9Theme shadow]);
        NSArray<NSString *> *rowVals = (row < (NSInteger)_cellCache.count) ? _cellCache[row] : nil;  // cached
        for (NSInteger e = 0; e < nCols; e++) {
            NSString *val = (rowVals && e < (NSInteger)rowVals.count) ? rowVals[e] : @"";
            DrawCellText(val, NSMakeRect([self envContentX:e], y, [self envWidth:e], kRowH), fg);
        }
        // The Secret column holds a live OS9Toggle subview (positioned in -layoutSecretToggles).
    }

    // "+ alias" row drawn only when dirty reaches it (unchanged by hover/selection -> skipped on 1-row redraw).
    if (NSIntersectsRect(dirty, NSMakeRect(0, nRows * kRowH, cw, kAddRowH))) {
        DrawPlus(NSMakeRect(8, nRows * kRowH + (kAddRowH - kGlyph) / 2, kGlyph, kGlyph), [OS9Theme shadow]);
        DrawCellText(StrGridAddAlias, NSMakeRect(kGlyph + 14, nRows * kRowH, _aliasW, kAddRowH), [OS9Theme shadow]);
    }
}

@end
