#import "widgets/OS9EnvGrid.h"
#import "theme/OS9Theme.h"
#import "widgets/OS9Scroller.h"
#import "dialogs/OS9Dialog.h"

// ---- Geometry ----
static const CGFloat kHeaderH  = 22;
static const CGFloat kRowH     = 24;
static const CGFloat kAliasW0  = 170;   // bề rộng cột Alias mặc định
static const CGFloat kColW0    = 150;   // bề rộng cột env mặc định
static const CGFloat kAddEnvW  = 30;    // cột "+" thêm env ở ngoài cùng phải header
static const CGFloat kAddRowH  = 24;    // hàng "+ alias" cuối bảng
static const CGFloat kGlyph    = 13;    // hot-zone glyph ×
static const CGFloat kMinColW  = 60;    // bề rộng cột tối thiểu khi kéo
static const CGFloat kGrabW    = 5;     // vùng bắt để kéo dãn cột (mỗi bên divider)

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

// Cắt chuỗi cho vừa maxW, thêm "…" (drawInRect KHÔNG tự truncate -> phải tự làm).
static NSString *Ellipsize(NSString *s, CGFloat maxW, NSDictionary *attrs) {
    if (maxW <= 0) return @"";
    if ([s sizeWithAttributes:attrs].width <= maxW) return s;
    NSString *e = @"…";
    NSUInteger lo = 0, hi = s.length;          // tìm số ký tự lớn nhất mà "<prefix>…" vẫn vừa
    while (hi > lo) {
        NSUInteger mid = (lo + hi + 1) / 2;
        NSString *cand = [[s substringToIndex:mid] stringByAppendingString:e];
        if ([cand sizeWithAttributes:attrs].width <= maxW) lo = mid; else hi = mid - 1;
    }
    return [[s substringToIndex:lo] stringByAppendingString:e];
}

// Vẽ 1 dòng text trong cell: CLIP + ellipsize + canh giữa theo chiều dọc.
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
- (void)drawBodyIn:(NSView *)v;
- (void)drawHeaderIn:(NSView *)v;
- (void)bodyMouseDown:(NSEvent *)e;
- (void)headerMouseDown:(NSEvent *)e;
- (void)setHoverRowFromBodyEvent:(NSEvent *)e;
- (void)headerMouseMoved:(NSEvent *)e;
- (void)headerMouseExited;
@end

@implementation OS9EnvGridBody
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }   // tự phủ trắng toàn bộ -> không ghosting khi resize/scroll
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }   // click đầu vẫn nhận (double-click ổn)
- (void)drawRect:(NSRect)r { [self.owner drawBodyIn:self]; }
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
    NSMutableArray<NSNumber *> *_colW;   // bề rộng mỗi cột env (song song _envNames)
    BOOL _autoFitCols;                   // YES: căn đều cột env theo bề rộng khả dụng
    NSInteger _hoverRow;
    NSInteger _hoverEnvCol;              // cột env đang hover ở header (-1 = không) -> hiện × xoá
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _baseDisplayName = @"Local";
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

// Viền răng cưa đen bao quanh bảng (giống OS9SerratedInset của các pane khác).
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
- (CGFloat)envContentX:(NSInteger)e {   // x đầu cột env index e (content coords)
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

// Căn đều bề rộng cột env để LẤP ĐẦY phần khả dụng (dư px dồn vào cột cuối).
- (void)applyEvenColumnsForWidth:(CGFloat)W {
    NSInteger n = _envNames.count;
    if (n == 0) return;
    CGFloat avail = W - _aliasW - kAddEnvW;
    CGFloat each = floor(avail / n);
    if (each < kMinColW) each = kMinColW;
    for (NSInteger i = 0; i < n; i++) {
        CGFloat w = each;
        if (i == n - 1) { CGFloat rem = avail - each * (n - 1); if (rem > w) w = rem; }  // cột cuối ăn phần dư
        _colW[i] = @(w);
    }
}

- (void)layout {
    [super layout];
    const CGFloat B = 2;   // chừa chỗ cho viền răng cưa
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
    [self layout];
}

- (void)setEnvNames:(NSArray<NSString *> *)e {
    NSInteger oldCount = _colW.count;
    _envNames = [e copy];
    // Số cột đổi (thêm/xoá env) -> căn đều lại; chỉ value/rename (cùng số cột) -> giữ bề rộng manual.
    if ((NSInteger)_envNames.count != oldCount) _autoFitCols = YES;
    while ((NSInteger)_colW.count < (NSInteger)_envNames.count) [_colW addObject:@(kColW0)];
    while ((NSInteger)_colW.count > (NSInteger)_envNames.count) [_colW removeLastObject];
    [self reloadData];
}
- (void)setAliases:(NSArray<NSString *> *)a { _aliases = [a copy]; [self reloadData]; }
- (void)setSelectedRow:(NSInteger)r { _selectedRow = r; [_body setNeedsDisplay:YES]; }

- (NSString *)displayForEnv:(NSInteger)e {   // e = index 0-based trong _envNames
    if (e == 0) return _baseDisplayName ?: @"Local";
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
- (NSRect)closeBoxInRect:(NSRect)r {   // × ở góc phải header env
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

    CGFloat dx = -[self scrollX];   // header cuộn ngang đồng bộ body; sticky theo chiều DỌC.
    NSColor *fg = [OS9Theme frame];

    DrawCellText(@"Alias", NSMakeRect(dx, 0, _aliasW, kHeaderH), fg);
    [self drawVDivAt:_aliasW + dx height:kHeaderH];

    for (NSInteger e = 0; e < (NSInteger)_envNames.count; e++) {
        NSRect r = [self envRectAtIndex:e height:kHeaderH];
        r.origin.x += dx;
        [self drawVDivAt:NSMaxX(r) height:kHeaderH];
        BOOL showX = (e != 0 && e == _hoverEnvCol);   // × chỉ hiện khi hover cột đó (giống alias)
        NSRect txt = r;
        if (showX) txt.size.width -= (kGlyph + 6);     // chừa chỗ × khi hover
        DrawCellText([self displayForEnv:e], txt, fg);
        if (showX) DrawClose([self closeBoxInRect:r], [OS9Theme shadow]);
    }
    CGFloat addX = _aliasW + dx;
    for (NSInteger i = 0; i < (NSInteger)_envNames.count; i++) addX += [self envWidth:i];
    DrawPlus(NSInsetRect(NSMakeRect(addX, 0, kAddEnvW, kHeaderH), 8, 4), [OS9Theme frame]);

    [self drawHDivAt:kHeaderH width:b.size.width];
}

#pragma mark body drawing

- (void)drawBodyIn:(NSView *)v {
    [[NSColor whiteColor] set];
    NSRectFill(v.bounds);

    NSInteger nRows = _aliases.count;
    NSInteger nCols = _envNames.count;
    CGFloat cw = [self contentWidth];

    for (NSInteger row = 0; row < nRows; row++) {
        NSRect rr = NSMakeRect(0, row * kRowH, cw, kRowH);
        if (row == _selectedRow) { [[OS9Theme accent] set]; NSRectFill(rr); }
        else if (row % 2 == 1) { [[OS9Theme rowSelectionGray] set]; NSRectFill(rr); }
    }
    [[OS9Theme face] set];
    NSRectFill(NSMakeRect(0, nRows * kRowH, cw, kAddRowH));

    // lưới dọc.
    [self drawVDivAt:_aliasW height:[self bodyHeight]];
    for (NSInteger e = 0; e < nCols; e++)
        [self drawVDivAt:[self envContentX:e] + [self envWidth:e] height:[self bodyHeight]];
    // lưới ngang.
    for (NSInteger row = 1; row <= nRows; row++) {
        [NSGraphicsContext saveGraphicsState];
        [[NSGraphicsContext currentContext] setShouldAntialias:NO];
        [[OS9Theme rowSelectionGray] set];
        NSRectFill(NSMakeRect(0, row * kRowH, cw, 1));
        [NSGraphicsContext restoreGraphicsState];
    }

    for (NSInteger row = 0; row < nRows; row++) {
        BOOL sel = (row == _selectedRow);
        NSColor *fg = sel ? [NSColor whiteColor] : [OS9Theme frame];
        CGFloat y = row * kRowH;
        NSRect aliasCell = NSMakeRect(0, y, _aliasW, kRowH);
        if (row == _hoverRow || sel) aliasCell.size.width -= (kGlyph + 6);  // chừa × xoá alias
        DrawCellText(_aliases[row], aliasCell, fg);
        if (row == _hoverRow || sel)
            DrawClose([self closeBoxInAliasRowAtY:y], sel ? [NSColor whiteColor] : [OS9Theme shadow]);
        for (NSInteger e = 0; e < nCols; e++) {
            NSString *val = [self.delegate envGrid:self valueForAlias:_aliases[row] env:_envNames[e]] ?: @"";
            DrawCellText(val, NSMakeRect([self envContentX:e], y, [self envWidth:e], kRowH), fg);
        }
    }

    DrawPlus(NSMakeRect(8, nRows * kRowH + (kAddRowH - kGlyph) / 2, kGlyph, kGlyph), [OS9Theme shadow]);
    DrawCellText(@"Thêm alias", NSMakeRect(kGlyph + 14, nRows * kRowH, _aliasW, kAddRowH), [OS9Theme shadow]);
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

- (EnvHit)hitHeader:(NSPoint)p {   // p trong content coords
    EnvHit h = {EnvZoneNone, -1, -1};
    if (p.x < _aliasW) return h;
    CGFloat addX = [self envContentX:_envNames.count];
    if (p.x >= addX && p.x < addX + kAddEnvW) { h.zone = EnvZoneAddEnv; return h; }
    for (NSInteger e = 0; e < (NSInteger)_envNames.count; e++) {
        NSRect r = [self envRectAtIndex:e height:kHeaderH];
        if (p.x < NSMinX(r) || p.x >= NSMaxX(r)) continue;
        h.col = e;
        if (e != 0 && NSPointInRect(p, [self closeBoxInRect:r])) { h.zone = EnvZoneDeleteEnv; return h; }
        if (e != 0) h.zone = EnvZoneHeaderName;   // cột base không rename
        break;
    }
    return h;
}

// Divider nào (để kéo dãn) gần content-x? trả -2 = cột Alias; e>=0 = cột env e; -1 = không.
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
    // Ưu tiên: kéo dãn cột nếu bấm trúng divider.
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
    // cột env đang hover -> hiện × (bỏ qua cột base index 0).
    NSInteger hov = -1;
    for (NSInteger en = 1; en < (NSInteger)_envNames.count; en++) {
        NSRect r = [self envRectAtIndex:en height:kHeaderH];
        if (cx >= NSMinX(r) && cx < NSMaxX(r)) { hov = en; break; }
    }
    if (hov != _hoverEnvCol) { _hoverEnvCol = hov; [_header setNeedsDisplay:YES]; }
}

- (void)headerMouseExited {
    if (_hoverEnvCol != -1) { _hoverEnvCol = -1; [_header setNeedsDisplay:YES]; }
}

// Vòng lặp kéo dãn cột (giống OS9Divider): cập nhật bề rộng tới khi nhả chuột.
- (void)runResizeDrag:(NSInteger)target {
    _autoFitCols = NO;   // user nắm quyền chỉnh bề rộng -> ngừng auto-fit (tới khi thêm/xoá env)
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
        [_body displayIfNeeded];     // vòng lặp modal: vẽ ngay, tránh ghosting text cũ
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
    if (old != _hoverRow) [_body setNeedsDisplay:YES];
}

#pragma mark editing (OS9Dialog prompt — đáng tin trong config pane nhúng)

- (void)commitEditing { /* edit dạng modal: không có gì treo */ }

static NSString *Trim(NSString *s) {
    return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

- (NSString *)promptTitle:(NSString *)title default:(NSString *)def
                 validate:(NSString *(^)(NSString *))v {
    return [OS9Dialog promptWithTitle:title message:nil defaultText:(def ?: @"")
                          placeholder:nil okButton:@"OK" cancelButton:@"Cancel"
                             validate:v parent:self.window];
}

// --- Sửa value (cho phép rỗng) ---
- (void)promptEditValueAtRow:(NSInteger)row col:(NSInteger)col {
    NSString *alias = _aliases[row], *env = _envNames[col];
    NSString *cur = [self.delegate envGrid:self valueForAlias:alias env:env] ?: @"";
    NSString *nv = [self promptTitle:[NSString stringWithFormat:@"%@ · %@", [self displayForEnv:col], alias]
                             default:cur validate:nil];
    if (nv != nil) [self.delegate envGrid:self setValue:nv forAlias:alias env:env];
}

// --- Rename alias (chặn rỗng + trùng) ---
- (void)promptRenameAliasAtRow:(NSInteger)row {
    NSString *old = _aliases[row];
    __weak OS9EnvGrid *ws = self;
    NSString *nn = [self promptTitle:@"Đổi tên alias" default:old validate:^NSString *(NSString *s) {
        NSString *t = Trim(s);
        if (!t.length) return @"Tên không được rỗng";
        if (![t isEqualToString:old] && [ws.aliases containsObject:t]) return @"Alias đã tồn tại";
        return nil;
    }];
    NSString *t = Trim(nn);
    if (nn && t.length && ![t isEqualToString:old]) [self.delegate envGrid:self renameAlias:old to:t];
}

// --- Rename env (chặn rỗng + trùng + không cho trùng nhãn base) ---
- (void)promptRenameEnvAtCol:(NSInteger)col {
    if (col == 0) return;   // cột base không đổi tên
    NSString *old = _envNames[col];
    NSString *nn = [self promptTitle:@"Đổi tên environment" default:old
                            validate:[self envNameValidatorExcluding:old]];
    NSString *t = Trim(nn);
    if (nn && t.length && ![t isEqualToString:old]) [self.delegate envGrid:self renameEnv:old to:t];
}

// --- Thêm env (prompt tên + chặn rỗng/trùng -> không tạo nếu sai) ---
- (void)promptAddEnv {
    NSString *nn = [self promptTitle:@"Environment mới" default:@""
                            validate:[self envNameValidatorExcluding:nil]];
    NSString *t = Trim(nn);
    if (nn && t.length) [self.delegate envGrid:self addEnvNamed:t];
}

// --- Thêm alias (prompt tên + chặn rỗng/trùng) ---
- (void)promptAddAlias {
    __weak OS9EnvGrid *ws = self;
    NSString *nn = [self promptTitle:@"Alias mới" default:@"" validate:^NSString *(NSString *s) {
        NSString *t = Trim(s);
        if (!t.length) return @"Tên không được rỗng";
        if ([ws.aliases containsObject:t]) return @"Alias đã tồn tại";
        return nil;
    }];
    NSString *t = Trim(nn);
    if (nn && t.length) [self.delegate envGrid:self addAliasNamed:t];
}

// Validator tên env dùng chung cho add/rename: non-empty, không trùng env khác,
// không trùng KEY base ("Global") lẫn NHÃN base ("Local").
- (NSString * (^)(NSString *))envNameValidatorExcluding:(NSString *)exclude {
    __weak OS9EnvGrid *ws = self;
    return ^NSString *(NSString *s) {
        NSString *t = Trim(s);
        if (!t.length) return @"Tên không được rỗng";
        if ([t caseInsensitiveCompare:(ws.baseDisplayName ?: @"Local")] == NSOrderedSame ||
            [t caseInsensitiveCompare:@"Global"] == NSOrderedSame)
            return @"Tên này dành riêng cho cột nền";
        for (NSString *n in ws.envNames)
            if (![n isEqualToString:exclude] && [n isEqualToString:t]) return @"Environment đã tồn tại";
        return nil;
    };
}

@end
