#import "widgets/OS9EnvGrid.h"
#import "app/AppStrings.h"
#import "theme/OS9Theme.h"
#import "widgets/OS9Scroller.h"
#import "dialogs/OS9Dialog.h"

// ---- Geometry ----
static const CGFloat kHeaderH  = 22;
static const CGFloat kRowH     = 24;
static const CGFloat kAliasW0  = 170;   // default Alias column width
static const CGFloat kColW0    = 150;   // default env column width
static const CGFloat kAddEnvW  = 30;    // "+" add-env column at far right of header
static const CGFloat kAddRowH  = 24;    // "+ alias" row at bottom of table
static const CGFloat kGlyph    = 13;    // × glyph hot-zone
static const CGFloat kMinColW  = 60;    // minimum column width when dragging
static const CGFloat kGrabW    = 5;     // grab zone for column resize (each side of divider)

typedef NS_ENUM(NSInteger, EnvZone) {
    EnvZoneNone = 0,
    EnvZoneCellValue,
    EnvZoneAliasName,
    EnvZoneHeaderName,
    EnvZoneDeleteEnv,
    EnvZoneDeleteAlias,
    EnvZoneAddEnv,
    EnvZoneAddAlias,
};

typedef struct { EnvZone zone; NSInteger row; NSInteger col; } EnvHit;

static void DrawClose(NSRect box, NSColor *c) {
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
static void DrawPlus(NSRect box, NSColor *c) {
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

static NSDictionary *TextAttrs(NSColor *fg) {
    return @{ NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : fg };
}

// Truncate string to fit maxW, appending "…" (drawInRect does NOT truncate -> do it ourselves).
static NSString *Ellipsize(NSString *s, CGFloat maxW, NSDictionary *attrs) {
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
static void DrawCellText(NSString *s, NSRect cell, NSColor *fg) {
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

#pragma mark - internal flipped subviews

@class OS9EnvGrid;
@interface OS9EnvGridBody : NSView
@property(nonatomic, weak) OS9EnvGrid *owner;
@end
@interface OS9EnvGridHeader : NSView
@property(nonatomic, weak) OS9EnvGrid *owner;
@end

@interface OS9EnvGrid ()
- (void)drawBodyIn:(NSView *)v dirty:(NSRect)dirty;
- (void)drawHeaderIn:(NSView *)v;
- (void)bodyMouseDown:(NSEvent *)e;
- (void)headerMouseDown:(NSEvent *)e;
- (void)setHoverRowFromBodyEvent:(NSEvent *)e;
- (void)headerMouseMoved:(NSEvent *)e;
- (void)headerMouseExited;
- (void)invalidateBodyRow:(NSInteger)row;   // invalidate EXACTLY 1 body row (hover/selection)
@end

@implementation OS9EnvGridBody
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }   // fully paints white -> no ghosting on resize/scroll
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }   // first click still registers (double-click works)
- (void)drawRect:(NSRect)r { [self.owner drawBodyIn:self dirty:r]; }
- (void)mouseDown:(NSEvent *)e { [self.owner bodyMouseDown:e]; }
- (void)mouseMoved:(NSEvent *)e { [self.owner setHoverRowFromBodyEvent:e]; }
- (void)mouseExited:(NSEvent *)e { [self.owner setHoverRowFromBodyEvent:nil]; }
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *ta in [self.trackingAreas copy]) [self removeTrackingArea:ta];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow)
               owner:self userInfo:nil]];
}
@end

@implementation OS9EnvGridHeader
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (void)drawRect:(NSRect)r { [self.owner drawHeaderIn:self]; }
- (void)mouseDown:(NSEvent *)e { [self.owner headerMouseDown:e]; }
- (void)mouseMoved:(NSEvent *)e { [self.owner headerMouseMoved:e]; }
- (void)mouseExited:(NSEvent *)e { [self.owner headerMouseExited]; }
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *ta in [self.trackingAreas copy]) [self removeTrackingArea:ta];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow)
               owner:self userInfo:nil]];
}
@end

#pragma mark - OS9EnvGrid

@implementation OS9EnvGrid {
    OS9EnvGridHeader *_header;
    NSScrollView *_scroll;
    OS9EnvGridBody *_body;

    CGFloat _aliasW;
    NSMutableArray<NSNumber *> *_colW;   // width of each env column (parallel to _envNames)
    BOOL _autoFitCols;                   // YES: distribute env columns evenly over available width
    NSInteger _hoverRow;
    NSInteger _hoverEnvCol;              // env column hovered in header (-1 = none) -> shows × delete
    NSArray<NSArray<NSString *> *> *_cellCache;   // H8: value matrix [row][col], rebuilt on reloadData
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _baseDisplayName = StrEnvLocal;
        _selectedRow = -1;
        _hoverRow = -1;
        _hoverEnvCol = -1;
        _envNames = @[];
        _aliases = @[];
        _aliasW = kAliasW0;
        _colW = [NSMutableArray array];
        _autoFitCols = YES;

        _header = [[OS9EnvGridHeader alloc] initWithFrame:NSZeroRect];
        _header.owner = self;
        [self addSubview:_header];

        _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        _scroll.hasVerticalScroller = YES;
        _scroll.hasHorizontalScroller = YES;
        _scroll.autohidesScrollers = YES;
        _scroll.scrollerStyle = NSScrollerStyleOverlay;
        _scroll.scrollerKnobStyle = NSScrollerKnobStyleDefault;
        _scroll.drawsBackground = NO;
        _scroll.verticalScroller = [[OS9Scroller alloc] initWithFrame:NSMakeRect(0, 0, 16, 100)];
        _scroll.horizontalScroller = [[OS9Scroller alloc] initWithFrame:NSMakeRect(0, 0, 100, 16)];
        _body = [[OS9EnvGridBody alloc] initWithFrame:NSZeroRect];
        _body.owner = self;
        _scroll.documentView = _body;
        [self addSubview:_scroll];

        _scroll.contentView.postsBoundsChangedNotifications = YES;
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(scrollChanged:)
                                                     name:NSViewBoundsDidChangeNotification
                                                   object:_scroll.contentView];
    }
    return self;
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }

// Black serrated border around the table (like OS9SerratedInset on other panes).
- (void)drawRect:(NSRect)dirty {
    [[OS9Theme face] set];
    NSRectFill(self.bounds);
    NSBezierPath *p = [OS9Theme serratedPathInRect:NSInsetRect(self.bounds, 1, 1)];
    [[NSColor whiteColor] set];
    [p fill];
    [[OS9Theme frame] set];
    p.lineWidth = 1.0;
    [p stroke];
}

- (void)scrollChanged:(NSNotification *)n { [_header setNeedsDisplay:YES]; }

#pragma mark geometry (per-column widths)

- (CGFloat)envWidth:(NSInteger)e { return (e < (NSInteger)_colW.count) ? _colW[e].doubleValue : kColW0; }
- (CGFloat)envContentX:(NSInteger)e {   // x at start of env column index e (content coords)
    CGFloat x = _aliasW;
    for (NSInteger i = 0; i < e; i++) x += [self envWidth:i];
    return x;
}
- (NSRect)envRectAtIndex:(NSInteger)e height:(CGFloat)h {
    return NSMakeRect([self envContentX:e], 0, [self envWidth:e], h);
}
- (CGFloat)contentWidth {
    CGFloat x = _aliasW;
    for (NSInteger i = 0; i < (NSInteger)_envNames.count; i++) x += [self envWidth:i];
    return x + kAddEnvW;
}
- (CGFloat)bodyHeight { return _aliases.count * kRowH + kAddRowH; }
- (CGFloat)scrollX { return _scroll.contentView.bounds.origin.x; }

// Distribute env column widths evenly to FILL the available space (leftover px go to last column).
- (void)applyEvenColumnsForWidth:(CGFloat)W {
    NSInteger n = _envNames.count;
    if (n == 0) return;
    CGFloat avail = W - _aliasW - kAddEnvW;
    CGFloat each = floor(avail / n);
    if (each < kMinColW) each = kMinColW;
    for (NSInteger i = 0; i < n; i++) {
        CGFloat w = each;
        if (i == n - 1) { CGFloat rem = avail - each * (n - 1); if (rem > w) w = rem; }  // last column takes the remainder
        _colW[i] = @(w);
    }
}

- (void)layout {
    [super layout];
    const CGFloat B = 2;   // leave room for the serrated border
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    CGFloat innerW = W - 2 * B;
    if (_autoFitCols) [self applyEvenColumnsForWidth:innerW];
    _header.frame = NSMakeRect(B, B, innerW, kHeaderH);
    _scroll.frame = NSMakeRect(B, B + kHeaderH, innerW, H - 2 * B - kHeaderH);
    CGFloat docW = MAX([self contentWidth], _scroll.contentView.bounds.size.width);
    CGFloat docH = MAX([self bodyHeight], _scroll.contentView.bounds.size.height);
    _body.frame = NSMakeRect(0, 0, docW, docH);
    [_header setNeedsDisplay:YES];
    [_body setNeedsDisplay:YES];
}

- (void)reloadData {
    if (_selectedRow >= (NSInteger)_aliases.count) _selectedRow = -1;
    [self rebuildCellCache];   // H8: query the delegate ONCE per data change, not once per repaint
    [self layout];
}

// H8: snapshot every (alias, env) value so drawBodyIn reads a cached matrix instead of calling the
// delegate (which builds a stringWithFormat dictionary key) for every visible cell on every repaint —
// the hot path during resize-drag (displayIfNeeded per mouse-move).
- (void)rebuildCellCache {
    NSMutableArray<NSArray<NSString *> *> *rows = [NSMutableArray arrayWithCapacity:_aliases.count];
    for (NSString *alias in _aliases) {
        NSMutableArray<NSString *> *cols = [NSMutableArray arrayWithCapacity:_envNames.count];
        for (NSString *env in _envNames)
            [cols addObject:([self.delegate envGrid:self valueForAlias:alias env:env] ?: @"")];
        [rows addObject:cols];
    }
    _cellCache = rows;
}

- (void)setEnvNames:(NSArray<NSString *> *)e {
    NSInteger oldCount = _colW.count;
    _envNames = [e copy];
    // Column count changed (add/delete env) -> redistribute evenly; value/rename only (same count) -> keep manual widths.
    if ((NSInteger)_envNames.count != oldCount) _autoFitCols = YES;
    while ((NSInteger)_colW.count < (NSInteger)_envNames.count) [_colW addObject:@(kColW0)];
    while ((NSInteger)_colW.count > (NSInteger)_envNames.count) [_colW removeLastObject];
    [self reloadData];
}
- (void)setAliases:(NSArray<NSString *> *)a { _aliases = [a copy]; [self reloadData]; }
- (void)setSelectedRow:(NSInteger)r {
    if (_selectedRow == r) return;
    NSInteger old = _selectedRow;
    _selectedRow = r;
    [self invalidateBodyRow:old];   // redraw only old + new row (not the whole table)
    [self invalidateBodyRow:r];
}

// Invalidate the EXACT rect of 1 body row -> drawBodyIn re-queries only that row's cells.
- (void)invalidateBodyRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_aliases.count) return;
    [_body setNeedsDisplayInRect:NSMakeRect(0, row * kRowH, _body.bounds.size.width, kRowH)];
}

- (NSString *)displayForEnv:(NSInteger)e {   // e = 0-based index into _envNames (every env is its own name)
    return _envNames[e];
}

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
        BOOL showX = (e == _hoverEnvCol);   // × shows when hovering any column (all envs deletable)
        NSRect txt = r;
        if (showX) txt.size.width -= (kGlyph + 6);     // leave room for × on hover
        DrawCellText([self displayForEnv:e], txt, fg);
        if (showX) DrawClose([self closeBoxInRect:r], [OS9Theme shadow]);
    }
    CGFloat addX = _aliasW + dx;
    for (NSInteger i = 0; i < (NSInteger)_envNames.count; i++) addX += [self envWidth:i];
    DrawPlus(NSInsetRect(NSMakeRect(addX, 0, kAddEnvW, kHeaderH), 8, 4), [OS9Theme frame]);

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

    // vertical grid.
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
        NSArray<NSString *> *rowVals = (row < (NSInteger)_cellCache.count) ? _cellCache[row] : nil;  // H8: cached
        for (NSInteger e = 0; e < nCols; e++) {
            NSString *val = (rowVals && e < (NSInteger)rowVals.count) ? rowVals[e] : @"";
            DrawCellText(val, NSMakeRect([self envContentX:e], y, [self envWidth:e], kRowH), fg);
        }
    }

    // "+ alias" row drawn only when dirty reaches it (unchanged by hover/selection -> skipped on 1-row redraw).
    if (NSIntersectsRect(dirty, NSMakeRect(0, nRows * kRowH, cw, kAddRowH))) {
        DrawPlus(NSMakeRect(8, nRows * kRowH + (kAddRowH - kGlyph) / 2, kGlyph, kGlyph), [OS9Theme shadow]);
        DrawCellText(StrGridAddAlias, NSMakeRect(kGlyph + 14, nRows * kRowH, _aliasW, kAddRowH), [OS9Theme shadow]);
    }
}

#pragma mark hit-testing

- (EnvHit)hitBody:(NSPoint)p {
    EnvHit h = {EnvZoneNone, -1, -1};
    NSInteger nRows = _aliases.count;
    if (p.y >= nRows * kRowH && p.y < nRows * kRowH + kAddRowH) { h.zone = EnvZoneAddAlias; return h; }
    NSInteger row = (NSInteger)(p.y / kRowH);
    if (row < 0 || row >= nRows) return h;
    h.row = row;
    if (p.x < _aliasW) {
        if (NSPointInRect(p, [self closeBoxInAliasRowAtY:row * kRowH])) { h.zone = EnvZoneDeleteAlias; return h; }
        h.zone = EnvZoneAliasName; return h;
    }
    for (NSInteger e = 0; e < (NSInteger)_envNames.count; e++) {
        CGFloat x0 = [self envContentX:e];
        if (p.x >= x0 && p.x < x0 + [self envWidth:e]) { h.zone = EnvZoneCellValue; h.col = e; break; }
    }
    return h;
}

- (EnvHit)hitHeader:(NSPoint)p {   // p in content coords
    EnvHit h = {EnvZoneNone, -1, -1};
    if (p.x < _aliasW) return h;
    CGFloat addX = [self envContentX:_envNames.count];
    if (p.x >= addX && p.x < addX + kAddEnvW) { h.zone = EnvZoneAddEnv; return h; }
    for (NSInteger e = 0; e < (NSInteger)_envNames.count; e++) {
        NSRect r = [self envRectAtIndex:e height:kHeaderH];
        if (p.x < NSMinX(r) || p.x >= NSMaxX(r)) continue;
        h.col = e;
        if (NSPointInRect(p, [self closeBoxInRect:r])) { h.zone = EnvZoneDeleteEnv; return h; }
        h.zone = EnvZoneHeaderName;   // every column is deletable + renamable
        break;
    }
    return h;
}

// Which resize divider is near content-x? returns -2 = Alias column; e>=0 = env column e; -1 = none.
- (NSInteger)resizeTargetForContentX:(CGFloat)x {
    if (fabs(x - _aliasW) <= kGrabW) return -2;
    for (NSInteger e = 0; e < (NSInteger)_envNames.count; e++) {
        CGFloat edge = [self envContentX:e] + [self envWidth:e];
        if (fabs(x - edge) <= kGrabW) return e;
    }
    return -1;
}

#pragma mark mouse

- (void)bodyMouseDown:(NSEvent *)e {
    NSPoint p = [_body convertPoint:e.locationInWindow fromView:nil];
    EnvHit h = [self hitBody:p];
    BOOL dbl = (e.clickCount >= 2);
    switch (h.zone) {
        case EnvZoneAddAlias: [self promptAddAlias]; break;
        case EnvZoneDeleteAlias: [self.delegate envGrid:self deleteAlias:_aliases[h.row]]; break;
        case EnvZoneAliasName:
            self.selectedRow = h.row;
            if (dbl) [self promptRenameAliasAtRow:h.row];
            break;
        case EnvZoneCellValue:
            self.selectedRow = h.row;
            if (dbl) [self promptEditValueAtRow:h.row col:h.col];
            break;
        default: self.selectedRow = -1; break;
    }
}

- (void)headerMouseDown:(NSEvent *)e {
    NSPoint raw = [_header convertPoint:e.locationInWindow fromView:nil];
    CGFloat cx = raw.x + [self scrollX];   // content coords
    // Priority: resize column if a divider was hit.
    NSInteger rt = [self resizeTargetForContentX:cx];
    if (rt != -1) { [self runResizeDrag:rt]; return; }

    EnvHit h = [self hitHeader:NSMakePoint(cx, raw.y)];
    switch (h.zone) {
        case EnvZoneAddEnv: [self promptAddEnv]; break;
        case EnvZoneDeleteEnv: [self.delegate envGrid:self deleteEnv:_envNames[h.col]]; break;
        case EnvZoneHeaderName:
            if (e.clickCount >= 2) [self promptRenameEnvAtCol:h.col];
            break;
        default: break;
    }
}

- (void)headerMouseMoved:(NSEvent *)e {
    NSPoint raw = [_header convertPoint:e.locationInWindow fromView:nil];
    CGFloat cx = raw.x + [self scrollX];
    if ([self resizeTargetForContentX:cx] != -1) [[NSCursor resizeLeftRightCursor] set];
    else [[NSCursor arrowCursor] set];
    // hovered env column -> show × (every column is deletable).
    NSInteger hov = -1;
    for (NSInteger en = 0; en < (NSInteger)_envNames.count; en++) {
        NSRect r = [self envRectAtIndex:en height:kHeaderH];
        if (cx >= NSMinX(r) && cx < NSMaxX(r)) { hov = en; break; }
    }
    if (hov != _hoverEnvCol) { _hoverEnvCol = hov; [_header setNeedsDisplay:YES]; }
}

- (void)headerMouseExited {
    if (_hoverEnvCol != -1) { _hoverEnvCol = -1; [_header setNeedsDisplay:YES]; }
}

// Column resize drag loop (like OS9Divider): update width until mouse-up.
- (void)runResizeDrag:(NSInteger)target {
    _autoFitCols = NO;   // user controls width -> stop auto-fit (until env added/removed)
    NSWindow *win = self.window;
    CGFloat orig = (target == -2) ? _aliasW : [self envWidth:target];
    NSPoint p0 = [_header convertPoint:[win mouseLocationOutsideOfEventStream] fromView:nil];
    CGFloat startX = p0.x + [self scrollX];
    [[NSCursor resizeLeftRightCursor] set];
    while (1) {
        NSEvent *ev = [win nextEventMatchingMask:(NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp)];
        NSPoint p = [_header convertPoint:ev.locationInWindow fromView:nil];
        CGFloat cur = p.x + [self scrollX];
        CGFloat w = MAX(kMinColW, orig + (cur - startX));
        if (target == -2) _aliasW = w;
        else if (target < (NSInteger)_colW.count) _colW[target] = @(w);
        [self layout];
        // L3: per-frame layout + redraw in this modal drag loop is acceptable for a config screen — and with
        // the H8 value-matrix cache the redraw no longer re-queries the delegate per cell.
        [_body displayIfNeeded];     // modal loop: draw immediately, avoid ghosting of old text
        [_header displayIfNeeded];
        if (ev.type == NSEventTypeLeftMouseUp) break;
    }
    [[NSCursor arrowCursor] set];
}

- (void)setHoverRowFromBodyEvent:(NSEvent *)e {
    NSInteger old = _hoverRow;
    if (!e) { _hoverRow = -1; }
    else {
        NSPoint p = [_body convertPoint:e.locationInWindow fromView:nil];
        NSInteger row = (NSInteger)(p.y / kRowH);
        _hoverRow = (row >= 0 && row < (NSInteger)_aliases.count) ? row : -1;
    }
    if (old != _hoverRow) { [self invalidateBodyRow:old]; [self invalidateBodyRow:_hoverRow]; }
}

#pragma mark editing (OS9Dialog prompt — reliable in embedded config pane)

- (void)commitEditing { /* modal edit: nothing pending */ }

static NSString *Trim(NSString *s) {
    return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

- (NSString *)promptTitle:(NSString *)title default:(NSString *)def
                 validate:(NSString *(^)(NSString *))v {
    return [OS9Dialog promptWithTitle:title message:nil defaultText:(def ?: @"")
                          placeholder:nil okButton:StrOK cancelButton:StrCancel
                             validate:v parent:self.window];
}

// --- Edit value (empty allowed) ---
- (void)promptEditValueAtRow:(NSInteger)row col:(NSInteger)col {
    NSString *alias = _aliases[row], *env = _envNames[col];
    NSString *cur = [self.delegate envGrid:self valueForAlias:alias env:env] ?: @"";
    NSString *nv = [self promptTitle:[NSString stringWithFormat:StrFmtEnvAliasTitle, [self displayForEnv:col], alias]
                             default:cur validate:nil];
    if (nv != nil) [self.delegate envGrid:self setValue:nv forAlias:alias env:env];
}

// --- Rename alias (block empty + duplicate) ---
- (void)promptRenameAliasAtRow:(NSInteger)row {
    NSString *old = _aliases[row];
    __weak OS9EnvGrid *ws = self;
    NSString *nn = [self promptTitle:StrDlgRenameAlias default:old validate:^NSString *(NSString *s) {
        NSString *t = Trim(s);
        if (!t.length) return StrValNameEmpty;
        if (![t isEqualToString:old] && [ws.aliases containsObject:t]) return StrValAliasExists;
        return nil;
    }];
    NSString *t = Trim(nn);
    if (nn && t.length && ![t isEqualToString:old]) [self.delegate envGrid:self renameAlias:old to:t];
}

// --- Rename env (block empty + duplicate) — any column is renamable ---
- (void)promptRenameEnvAtCol:(NSInteger)col {
    NSString *old = _envNames[col];
    NSString *nn = [self promptTitle:StrDlgRenameEnv default:old
                            validate:[self envNameValidatorExcluding:old]];
    NSString *t = Trim(nn);
    if (nn && t.length && ![t isEqualToString:old]) [self.delegate envGrid:self renameEnv:old to:t];
}

// --- Add env (prompt name + block empty/duplicate -> don't create if invalid) ---
- (void)promptAddEnv {
    NSString *nn = [self promptTitle:StrDlgNewEnv default:@""
                            validate:[self envNameValidatorExcluding:nil]];
    NSString *t = Trim(nn);
    if (nn && t.length) [self.delegate envGrid:self addEnvNamed:t];
}

// --- Add alias (prompt name + block empty/duplicate) ---
- (void)promptAddAlias {
    __weak OS9EnvGrid *ws = self;
    NSString *nn = [self promptTitle:StrDlgNewAlias default:@"" validate:^NSString *(NSString *s) {
        NSString *t = Trim(s);
        if (!t.length) return StrValNameEmpty;
        if ([ws.aliases containsObject:t]) return StrValAliasExists;
        return nil;
    }];
    NSString *t = Trim(nn);
    if (nn && t.length) [self.delegate envGrid:self addAliasNamed:t];
}

// Shared env-name validator for add/rename: non-empty + no duplicate of another env (no reserved names).
- (NSString * (^)(NSString *))envNameValidatorExcluding:(NSString *)exclude {
    __weak OS9EnvGrid *ws = self;
    return ^NSString *(NSString *s) {
        NSString *t = Trim(s);
        if (!t.length) return StrValNameEmpty;
        for (NSString *n in ws.envNames)
            if (![n isEqualToString:exclude] && [n isEqualToString:t]) return StrValEnvExists;
        return nil;
    };
}

@end
