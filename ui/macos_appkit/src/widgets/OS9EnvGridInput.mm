#import "widgets/OS9EnvGridInternal.h"
#import "app/AppStrings.h"
#import "dialogs/OS9Dialog.h"

typedef NS_ENUM(NSInteger, EnvZone) {
    EnvZoneNone = 0,
    EnvZoneCellValue,
    EnvZoneAliasName,
    EnvZoneHeaderName,
    EnvZoneDeleteEnv,
    EnvZoneDeleteAlias,
    EnvZoneAddEnv,
    EnvZoneAddAlias,
    EnvZoneToggleSecret,
};

typedef struct { EnvZone zone; NSInteger row; NSInteger col; } EnvHit;

@implementation OS9EnvGrid (Input)

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
    if (h.zone == EnvZoneNone) {   // trailing column (holds the secret toggle)
        CGFloat tx = [self trailingX];
        if (p.x >= tx && p.x < tx + [self trailingColW]) h.zone = EnvZoneToggleSecret;
    }
    return h;
}

- (EnvHit)hitHeader:(NSPoint)p {   // p in content coords
    EnvHit h = {EnvZoneNone, -1, -1};
    if (p.x < _aliasW) return h;
    CGFloat addX = [self trailingX];   // "+" sits in the trailing column
    if (p.x >= addX && p.x < addX + [self trailingColW]) { h.zone = EnvZoneAddEnv; return h; }
    for (NSInteger e = 0; e < (NSInteger)_envNames.count; e++) {
        NSRect r = [self envRectAtIndex:e height:kHeaderH];
        if (p.x < NSMinX(r) || p.x >= NSMaxX(r)) continue;
        h.col = e;
        BOOL protectedCol = (_protectedFirstColumn && e == 0);   // no × zone on protected col
        if (!protectedCol && NSPointInRect(p, [self closeBoxInRect:r])) { h.zone = EnvZoneDeleteEnv; return h; }
        h.zone = EnvZoneHeaderName;
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
        case EnvZoneToggleSecret:
            // The live OS9Toggle handles its own click; a click beside it just selects the row.
            self.selectedRow = h.row;
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
        // Per-frame layout+redraw in this modal drag loop is fine for a config screen — the value-matrix
        // cache means the redraw doesn't re-query the delegate per cell.
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

static NSString *Trim(NSString *s) {
    return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

- (NSString *)promptTitle:(NSString *)title default:(NSString *)def
                 validate:(NSString *(^)(NSString *))v {
    return [OS9Dialog promptWithTitle:title message:nil defaultText:(def ?: @"")
                          placeholder:nil okButton:StrOK cancelButton:StrCancel
                             validate:v parent:self.window];
}

// Edit value (empty allowed).
- (void)promptEditValueAtRow:(NSInteger)row col:(NSInteger)col {
    NSString *alias = _aliases[row], *env = _envNames[col];
    NSString *cur = [self.delegate envGrid:self valueForAlias:alias env:env] ?: @"";
    NSString *nv = [self promptTitle:[NSString stringWithFormat:StrFmtEnvAliasTitle, [self displayForEnv:col], alias]
                             default:cur validate:nil];
    if (nv != nil) [self.delegate envGrid:self setValue:nv forAlias:alias env:env];
}

// Rename alias (block empty + duplicate).
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

// Rename env (block empty + duplicate) — any column except the protected base.
- (void)promptRenameEnvAtCol:(NSInteger)col {
    if (_protectedFirstColumn && col == 0) return;   // reserved base keeps its name
    NSString *old = _envNames[col];
    NSString *nn = [self promptTitle:StrDlgRenameEnv default:old
                            validate:[self envNameValidatorExcluding:old]];
    NSString *t = Trim(nn);
    if (nn && t.length && ![t isEqualToString:old]) [self.delegate envGrid:self renameEnv:old to:t];
}

// Add env (block empty/duplicate -> don't create if invalid).
- (void)promptAddEnv {
    NSString *nn = [self promptTitle:StrDlgNewEnv default:@""
                            validate:[self envNameValidatorExcluding:nil]];
    NSString *t = Trim(nn);
    if (nn && t.length) [self.delegate envGrid:self addEnvNamed:t];
}

// Add alias (block empty/duplicate).
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

// Env-name validator for add/rename: non-empty + no duplicate. Protected base name also rejected
// case-insensitively (APFS filenames collide).
- (NSString * (^)(NSString *))envNameValidatorExcluding:(NSString *)exclude {
    __weak OS9EnvGrid *ws = self;
    return ^NSString *(NSString *s) {
        NSString *t = Trim(s);
        if (!t.length) return StrValNameEmpty;
        for (NSString *n in ws.envNames)
            if (![n isEqualToString:exclude] && [n isEqualToString:t]) return StrValEnvExists;
        if (ws.protectedFirstColumn && ws.envNames.count &&
            ![ws.envNames[0] isEqualToString:exclude] &&
            [ws.envNames[0] caseInsensitiveCompare:t] == NSOrderedSame) return StrValEnvExists;
        return nil;
    };
}

@end
