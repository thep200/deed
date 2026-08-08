#import "widgets/OS9EnvGrid.h"
#import "widgets/OS9EnvGridInternal.h"
#import "theme/OS9Theme.h"
#import "widgets/OS9Scroller.h"
#import "widgets/OS9Toggle.h"

#pragma mark - internal flipped subviews

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

@implementation OS9EnvGrid

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _selectedRow = -1;
        _hoverRow = -1;
        _hoverEnvCol = -1;
        _envNames = @[];
        _aliases = @[];
        _aliasW = kAliasW0;
        _colW = [NSMutableArray array];
        _secretToggles = [NSMutableArray array];
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
- (CGFloat)trailingX {   // x at start of the trailing column ("+" header + per-row secret toggles)
    CGFloat x = _aliasW;
    for (NSInteger i = 0; i < (NSInteger)_envNames.count; i++) x += [self envWidth:i];
    return x;
}
// Width of the secret toggle, sized to carry the label with a tight margin (two knob-cells).
- (CGFloat)secretToggleW {
    CGFloat lw = [kSecretLabel sizeWithAttributes:@{NSFontAttributeName : [OS9Theme uiFont]}].width;
    return (ceil(lw) + 5) * 2;   // ~2.5px label margin each side of the knob -> shorter toggle
}
// Trailing column width: the labelled toggle + side padding (also hosts the "+" add-env in the header).
- (CGFloat)trailingColW { return [self secretToggleW] + 12; }
- (CGFloat)contentWidth { return [self trailingX] + [self trailingColW]; }
// Centered OS9Toggle frame for the secret switch in the alias row `row` (body content coords).
- (NSRect)secretToggleFrameForRow:(NSInteger)row {
    CGFloat w = [self secretToggleW];
    CGFloat x = [self trailingX] + ([self trailingColW] - w) / 2;
    return NSMakeRect(floor(x), floor(row * kRowH + (kRowH - kToggleH) / 2), w, kToggleH);
}
- (CGFloat)bodyHeight { return _aliases.count * kRowH + kAddRowH; }
- (CGFloat)scrollX { return _scroll.contentView.bounds.origin.x; }

// Distribute env column widths evenly to FILL the available space (leftover px go to last column).
- (void)applyEvenColumnsForWidth:(CGFloat)W {
    NSInteger n = _envNames.count;
    if (n == 0) return;
    CGFloat avail = W - _aliasW - [self trailingColW];
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
    [self layoutSecretToggles];
    [_header setNeedsDisplay:YES];
    [_body setNeedsDisplay:YES];
}

// Sync the per-row secret toggles (create/remove to match alias count) and position them in the Secret
// column. Each toggle owns its click/animation and reports back via secretToggleClicked:.
- (void)layoutSecretToggles {
    NSInteger n = _aliases.count;
    while ((NSInteger)_secretToggles.count < n) {
        OS9Toggle *t = [[OS9Toggle alloc] initWithLabel:kSecretLabel target:self action:@selector(secretToggleClicked:)];
        [_secretToggles addObject:t];
        [_body addSubview:t];
    }
    while ((NSInteger)_secretToggles.count > n) {
        [_secretToggles.lastObject removeFromSuperview];
        [_secretToggles removeLastObject];
    }
    for (NSInteger row = 0; row < n; row++) {
        OS9Toggle *t = _secretToggles[row];
        t.tag = row;
        BOOL on = (row < (NSInteger)_secretCache.count) && _secretCache[row].boolValue;
        if (t.on != on) t.on = on;   // setOn snaps without animation — correct for (re)layout
        t.frame = [self secretToggleFrameForRow:row];
    }
}

- (void)secretToggleClicked:(OS9Toggle *)t {
    NSInteger row = t.tag;
    if (row < 0 || row >= (NSInteger)_aliases.count) return;
    BOOL on = t.on;   // OS9Toggle flips its state BEFORE sending the action
    self.selectedRow = row;
    [self.delegate envGrid:self setSecret:on forAlias:_aliases[row]];
    // Update the cache in place — do NOT reloadData here (it would destroy `t` mid-click).
    NSMutableArray<NSNumber *> *sc = [_secretCache mutableCopy] ?: [NSMutableArray array];
    while ((NSInteger)sc.count <= row) [sc addObject:@NO];
    sc[row] = @(on);
    _secretCache = sc;
}

- (void)reloadData {
    if (_selectedRow >= (NSInteger)_aliases.count) _selectedRow = -1;
    [self rebuildCellCache];   // query the delegate ONCE per data change, not once per repaint
    [self layout];
}

// Snapshot every (alias, env) value so drawBodyIn reads a cached matrix instead of calling the
// delegate (which builds a stringWithFormat dictionary key) for every visible cell on every repaint —
// the hot path during resize-drag (displayIfNeeded per mouse-move).
- (void)rebuildCellCache {
    NSMutableArray<NSArray<NSString *> *> *rows = [NSMutableArray arrayWithCapacity:_aliases.count];
    NSMutableArray<NSNumber *> *secrets = [NSMutableArray arrayWithCapacity:_aliases.count];
    for (NSString *alias in _aliases) {
        NSMutableArray<NSString *> *cols = [NSMutableArray arrayWithCapacity:_envNames.count];
        for (NSString *env in _envNames)
            [cols addObject:([self.delegate envGrid:self valueForAlias:alias env:env] ?: @"")];
        [rows addObject:cols];
        [secrets addObject:@([self.delegate envGrid:self isSecretForAlias:alias])];
    }
    _cellCache = rows;
    _secretCache = secrets;
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

// Stays in the primary @implementation: declared on the primary interface (-Wincomplete-implementation).
- (void)commitEditing { /* modal edit: nothing pending */ }

@end
