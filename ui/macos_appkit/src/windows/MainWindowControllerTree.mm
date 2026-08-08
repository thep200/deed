#import "windows/MainWindowControllerPrivate.h"

@implementation MainWindowController (Tree)

#pragma mark Collection / tree

- (void)openFolder:(id)sender {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES; p.canChooseFiles = NO; p.allowsMultipleSelection = NO;
    p.prompt = StrOpenCollection;
    if ([p runModal] == NSModalResponseOK) [self openCollectionRoot:p.URL.path];
}

// App-config defaults read from .env (DeedConfig). Used when config.json lacks a key,
// so users can tweak defaults via .env without editing code.
- (core::AppConfig)appDefaultsFromEnv {
    DeedConfig *dc = [DeedConfig shared];
    core::AppConfig d;
    d.fontName = S([dc stringFor:@"FONT_NAME" def:@""]);
    d.fontSize = (int)[dc intFor:@"FONT_SIZE" def:core::kDefaultFontSize];
    d.ramCacheSizeMb = (int)[dc intFor:@"RAM_CACHE_SIZE" def:core::kDefaultRamCacheSizeMb];
    d.diskCacheSizeMb = (int)[dc intFor:@"DISK_CACHE_SIZE" def:core::kDefaultDiskCacheSizeMb];
    return d;
}

- (void)openCollectionRoot:(NSString *)path {
    [self autosaveCurrent];
    [_expandedFolders removeAllObjects];   // new collection: reset fold state (folded by default)
    _root = path.UTF8String;
    // CoreApiClient owns its own stores + response cache. .env tunables (read by DeedConfig;
    // Core never reads .env) map into its Config.
    core::app::CoreApiClient::Config cfg;
    cfg.collectionRoot = _root;
    DeedConfig *dc = [DeedConfig shared];
    cfg.ramCacheMaxMb = (int)[dc intFor:@"RAM_CACHE_SIZE_MAX" def:0];
    cfg.ramCacheMinMb = (int)[dc intFor:@"RAM_CACHE_SIZE_MIN" def:0];
    cfg.diskCacheMaxMb = (int)[dc intFor:@"DISK_CACHE_SIZE_MAX" def:0];
    cfg.diskCacheMinMb = (int)[dc intFor:@"DISK_CACHE_SIZE_MIN" def:0];
    cfg.ramCacheThresholdKb = (int)[dc intFor:@"RAM_CACHE_THRESHOLD_KB" def:0];
    // WebSocket tunables from .env (0 -> WsSender default).
    cfg.wsPingIntervalMs = (int)[dc intFor:@"WS_PING_INTERVAL_MS" def:0];
    cfg.wsIdleTimeoutMs = (int)[dc intFor:@"WS_IDLE_TIMEOUT_MS" def:0];
    cfg.wsCloseTimeoutMs = (int)[dc intFor:@"WS_CLOSE_TIMEOUT_MS" def:0];
    cfg.wsConnectTimeoutMs = (int)[dc intFor:@"WS_CONNECT_TIMEOUT_MS" def:0];
    cfg.wsMaxFrameMb = (int)[dc intFor:@"WS_MAX_FRAME_MB" def:0];
    cfg.wsSendQueueMaxFrames = (int)[dc intFor:@"WS_SEND_QUEUE_MAX_FRAMES" def:0];
    cfg.wsSendQueueMaxMb = (int)[dc intFor:@"WS_SEND_QUEUE_MAX_MB" def:0];
    // gRPC streaming ceilings from .env (0 -> GrpcSender default).
    cfg.streamMaxEvents = (long long)[dc intFor:@"STREAM_MAX_EVENTS" def:0];
    cfg.streamMaxBytesMb = (int)[dc intFor:@"STREAM_MAX_BYTES_MB" def:0];
    // New-request per-request defaults from .env (Core never reads .env; 0 -> Core built-in 30-min timeout).
    cfg.defaultTimeoutMs = (long long)[dc intFor:@"DEFAULT_TIMEOUT_MS" def:0];
    cfg.defaultVerifyTls = [dc boolFor:@"VERIFY_TLS" def:YES];
    cfg.appDefaults = [self appDefaultsFromEnv];   // app-config defaults from .env
    _apiClient = core::app::CoreApiClient::create(std::move(cfg));
    _envVC = [[EnvWindowController alloc] initWithEnvRepo:&_apiClient->environments()
                                                 session:&_apiClient->session()];
    // Remember this folder in app-support so it reopens next time.
    try { core::AppConfig ac = _apiClient->appConfig().load(); ac.lastCollectionRoot = _root;
          _apiClient->appConfig().save(ac); } catch (...) {}
    _openButton.title = [self abbreviatePath:path];
    _openButton.toolTip = path;
    [self setHasRequest:NO];
    [self reloadTree];
    [self refreshEnvButton];

    try {
        std::string last = _apiClient->session().loadLastOpened();
        if (!last.empty()) {
            NSString *full = N(_root + "/" + last);
            if ([[NSFileManager defaultManager] fileExistsAtPath:full]) [self loadRequestAtRel:N(last)];
            else [self toast:[NSString stringWithFormat:StrFmtToastNotFound, last.c_str()]];
        }
    } catch (...) {}
}

// Resync _currentRel by stable id before writing: after rename/move the old path has changed
// -> avoid save writing to the old path and creating a "ghost" file. Returns NO if the open request was deleted.
- (BOOL)resyncCurrentRelById {
    if (_currentId.empty() || !_apiClient) return !_currentRel.empty();
    std::string rel = _apiClient->collection().findRelPathById(_currentId);
    if (rel.empty()) return NO;          // no longer on disk (deleted) -> don't recreate
    _currentRel = rel;
    return YES;
}

// Load a folder's children on demand (one readdir of that level, no recursion). Synchronous on the
// main thread, but it scans ONLY the expanded level — cost bounded to one directory's entries.
- (void)loadChildrenOf:(TreeItem *)folder {
    if (!folder || folder.childrenLoaded || !_apiClient) return;
    [folder.children removeAllObjects];
    try {
        for (const auto &c : _apiClient->collection().scanLevel(folder.relPath.UTF8String))
            [folder.children addObject:TreeItemFromNode(c)];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    folder.childrenLoaded = YES;
}

- (void)reloadTree {
    // reloadData resets selection -> the selected folder/request highlight vanishes on Cmd+S.
    // Save selected relPath BEFORE reload, restore by relPath AFTER reload (index may change).
    NSString *selRel = nil;
    NSInteger selRow = _tree.selectedRow;
    if (selRow >= 0) {
        id it = [_tree itemAtRow:selRow];
        if ([it isKindOfClass:[TreeItem class]]) selRel = ((TreeItem *)it).relPath;
    }

    [_roots removeAllObjects];
    if (_apiClient) {
        try {                               // scan ROOT level only; child folders folded by default
            for (const auto &c : _apiClient->collection().scanLevel(""))
                [_roots addObject:TreeItemFromNode(c)];
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    }
    [_tree reloadData];
    [self restoreExpansion:_roots];         // keep the folders the user had open across reload
    if (selRel.length) [self reselectTreeByRel:selRel];
}

// Re-select a node (folder or request) by relPath after reload, WITHOUT triggering auto-load.
- (void)reselectTreeByRel:(NSString *)rel {
    TreeItem *t = [self loadedItemForRel:rel inItems:_roots];
    if (!t) return;
    NSInteger row = [_tree rowForItem:t];
    if (row < 0) return;
    _revealingSelection = YES;
    [_tree selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    _revealingSelection = NO;
}

// Find a TreeItem (folder or request) by relPath among LOADED items (recursive). nil if not found.
- (TreeItem *)loadedItemForRel:(NSString *)rel inItems:(NSArray<TreeItem *> *)items {
    for (TreeItem *t in items) {
        if ([t.relPath isEqualToString:rel]) return t;
        if (t.isFolder && t.childrenLoaded) {
            TreeItem *found = [self loadedItemForRel:rel inItems:t.children];
            if (found) return found;
        }
    }
    return nil;
}

// Reopen folders present in _expandedFolders (lazy: expandItem triggers loading children).
- (void)restoreExpansion:(NSArray<TreeItem *> *)items {
    for (TreeItem *t in items) {
        if (!t.isFolder) continue;
        if ([_expandedFolders containsObject:t.relPath]) {
            [_tree expandItem:t];           // -> numberOfChildren loads children if needed
            [self restoreExpansion:t.children];  // recurse into the just-loaded children
        }
    }
}

// Find a folder TreeItem by relPath AMONG LOADED items (no disk read). nil if not loaded/closed.
- (TreeItem *)loadedFolderItemForRel:(NSString *)rel {
    if (rel.length == 0) return nil;
    NSMutableArray<TreeItem *> *stack = [_roots mutableCopy];
    while (stack.count) {
        TreeItem *t = stack.lastObject; [stack removeLastObject];
        if (!t.isFolder) continue;
        if ([t.relPath isEqualToString:rel]) return t;
        if (t.childrenLoaded) [stack addObjectsFromArray:t.children];
    }
    return nil;
}

// Rescan one level (rel; "" = root) then MERGE into `items` in place: keep old TreeItems matching by
// relPath (preserving loaded children + that branch's open state), create new only for new entries.
// Returns YES if the child SET/ORDER changed (item added/removed/reordered) -> the caller must re-query
// children (reloadChildren:YES). NO = same items in same order (only leaf metadata refreshed in place) ->
// the caller can just redraw the existing rows, avoiding a recursive subtree rebuild (perf).
- (BOOL)mergeScanLevel:(const std::string &)rel into:(NSMutableArray<TreeItem *> *)items {
    if (!_apiClient) return NO;
    std::vector<core::TreeNode> nodes;
    try { nodes = _apiClient->collection().scanLevel(rel); }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; return NO; }
    NSMutableArray<NSString *> *oldOrder = [NSMutableArray arrayWithCapacity:items.count];
    for (TreeItem *t in items) [oldOrder addObject:(t.relPath ?: @"")];
    NSMutableDictionary<NSString *, TreeItem *> *byRel = [NSMutableDictionary dictionary];
    for (TreeItem *t in items) if (t.relPath) byRel[t.relPath] = t;
    NSMutableArray<TreeItem *> *merged = [NSMutableArray arrayWithCapacity:nodes.size()];
    for (const auto &n : nodes) {
        TreeItem *fresh = TreeItemFromNode(n);
        TreeItem *existing = byRel[fresh.relPath];
        if (existing && existing.isFolder == fresh.isFolder) {
            // Update leaf metadata (name/badge may change) but KEEP children + childrenLoaded.
            existing.name = fresh.name;
            existing.requestId = fresh.requestId;
            existing.badge = fresh.badge;
            existing.mark = fresh.mark;
            existing.grpc = fresh.grpc;
            [merged addObject:existing];
        } else {
            [merged addObject:fresh];
        }
    }
    [items setArray:merged];
    if (merged.count != oldOrder.count) return YES;
    for (NSUInteger i = 0; i < merged.count; ++i)
        if (![(merged[i].relPath ?: @"") isEqualToString:oldOrder[i]]) return YES;
    return NO;
}

// Incremental update for one changed level — does NOT tear down the whole tree.
- (void)refreshTreeLevel:(NSString *)parentRel {
    if (!_apiClient) return;
    if (parentRel.length == 0) {                 // mutation at ROOT
        BOOL changed = [self mergeScanLevel:"" into:_roots];
        if (changed) {
            [_tree reloadData];                  // structure changed -> rebuild + restore open state
            [self restoreExpansion:_roots];      // expandItem -> NO re-scan (TreeItem keeps childrenLoaded)
        } else {
            for (TreeItem *c in _roots) [_tree reloadItem:c reloadChildren:NO]; // metadata only -> redraw rows
        }
        return;
    }
    TreeItem *f = [self loadedFolderItemForRel:parentRel];
    if (!f) {                                    // parent folder not loaded (closed) -> lazy load on open
        return;                                  // nothing to do: next expand will scanLevel
    }
    if (!f.childrenLoaded) {                      // collapsed -> children re-queried on next expand; cheap mark
        [_tree reloadItem:f reloadChildren:YES];
        return;
    }
    // Only re-query (and rebuild) f's whole subtree when the child set/order changed; otherwise just
    // redraw the existing direct rows in place — avoids tearing down every visible descendant row view.
    BOOL changed = [self mergeScanLevel:f.relPath.UTF8String into:f.children];
    if (changed) {
        [_tree reloadItem:f reloadChildren:YES]; // re-query only f's CHILDREN, other branches intact
        [self restoreExpansion:f.children];      // renamed children come back as fresh, collapsed items
    } else {
        for (TreeItem *c in f.children) [_tree reloadItem:c reloadChildren:NO];
    }
}

// Remap old->new relPath prefix in _expandedFolders (folder rename/move) — keep open state.
- (void)remapExpandedFoldersFrom:(NSString *)oldRel to:(NSString *)newRel {
    if (oldRel.length == 0) return;
    NSMutableSet<NSString *> *updated = [NSMutableSet set];
    NSString *prefix = [oldRel stringByAppendingString:@"/"];
    for (NSString *p in _expandedFolders) {
        if ([p isEqualToString:oldRel]) [updated addObject:newRel];
        else if ([p hasPrefix:prefix]) [updated addObject:[newRel stringByAppendingString:[p substringFromIndex:oldRel.length]]];
        else [updated addObject:p];
    }
    [_expandedFolders setSet:updated];
}
// Click on a folder -> fold/unfold.
- (void)treeClicked:(id)sender {
    NSInteger row = _tree.clickedRow;
    if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    if (!t.isFolder) return;
    if ([_tree isItemExpanded:t]) [_tree collapseItem:t];
    else [_tree expandItem:t];
}

// Double-click: empty area -> quick new HTTP request; on a row -> rename via Platinum prompt.
- (void)treeDoubleClicked:(id)sender {
    NSInteger row = _tree.clickedRow;
    if (row < 0) {   // empty area -> new HTTP request
        RequestTypeUi *http = TypeUiFor(core::RequestType::Http);
        [self createRequest:http.type name:http.defaultRequestName];
        return;
    }
    [self promptRenameItem:[_tree itemAtRow:row]];
}

// Rename via Platinum dialog: prompt + validate, then sync the filename.
- (void)promptRenameItem:(TreeItem *)t {
    if (!t || t.relPath.length == 0 || !_apiClient) return;
    NSString *newName = [OS9Dialog promptWithTitle:StrRename
                                           message:StrDlgRenameMsg
                                       defaultText:(t.name ?: @"")
                                       placeholder:StrPhName
                                          okButton:StrRename
                                      cancelButton:StrCancel
                                          validate:^NSString *(NSString *s) {
        NSString *tr = [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (tr.length == 0) return StrValNameEmpty;
        return nil;
    }
                                            parent:_window];
    if (!newName || [newName isEqualToString:t.name]) return;   // cancelled or unchanged
    [self autosaveCurrent];
    BOOL wasCurrent = (!_currentId.empty() && t.requestId.length && S(t.requestId) == _currentId);
    NSString *oldRel = t.relPath;
    NSString *parentRel = [oldRel stringByDeletingLastPathComponent];
    try {
        std::string newRel = _apiClient->collection().rename(oldRel.UTF8String, newName.UTF8String);
        if (t.isFolder) [self remapExpandedFoldersFrom:oldRel to:N(newRel)];  // keep open state
        if (wasCurrent) {
            // Sync the name into the open model: otherwise a later Save writes the OLD name -> filename rollback.
            if (_model) _model = _model->withName(newName.UTF8String);
            _currentRel = newRel;
            [self updateTitle];
        }
        [self refreshTreeLevel:parentRel];   // rescan only the level holding the item, not the whole tree
        if (wasCurrent) [self revealAndSelectRequestById:N(_currentId) relPath:N(_currentRel)];
        [self toastOk:StrToastRenamed];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

- (void)outlineViewItemDidExpand:(NSNotification *)n {
    TreeItem *t = n.userInfo[@"NSObject"];
    if (t.relPath) [_expandedFolders addObject:t.relPath];
    [self refreshDisclosureForItem:t expanded:YES];   // flip triangle ▷ -> ▽
}
- (void)outlineViewItemDidCollapse:(NSNotification *)n {
    TreeItem *t = n.userInfo[@"NSObject"];
    if (t.relPath) [_expandedFolders removeObject:t.relPath];
    [self refreshDisclosureForItem:t expanded:NO];     // flip triangle ▽ -> ▷
}

- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    if (item == nil) return _roots.count;
    TreeItem *t = item;
    if (!t.isFolder) return 0;
    if (!t.childrenLoaded) [self loadChildrenOf:t];   // lazy: load only when counting/displaying needs it
    return t.children.count;
}
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)idx ofItem:(id)item {
    if (item == nil) return _roots[idx];
    TreeItem *t = item;
    if (!t.childrenLoaded) [self loadChildrenOf:t];
    return t.children[idx];
}
// Folders are ALWAYS expandable (cheap, no disk read); requests are not.
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item { return ((TreeItem *)item).isFolder; }
// Row view self-draws gray background when selected (NSOutlineView virtualization/reuse).
- (NSTableRowView *)outlineView:(NSOutlineView *)ov rowViewForItem:(id)item {
    OS9RowView *rv = [ov makeViewWithIdentifier:@"os9row" owner:self];
    if (!rv) { rv = [[OS9RowView alloc] initWithFrame:NSZeroRect]; rv.identifier = @"os9row"; }
    return rv;
}
- (NSView *)outlineView:(NSOutlineView *)ov viewForTableColumn:(NSTableColumn *)col item:(id)item {
    TreeItem *t = item;
    TreeCellView *cell = [ov makeViewWithIdentifier:@"treecell" owner:self];
    if (!cell) {
        cell = [[TreeCellView alloc] initWithFrame:NSMakeRect(0, 0, col.width, 18)];
        cell.identifier = @"treecell";
        cell.translatesAutoresizingMaskIntoConstraints = YES;
    }
    cell.isFolder = t.isFolder;
    cell.isExpanded = t.isFolder && [ov isItemExpanded:item];   // ▽/▷ triangle
    // Request: method in its own column (mark) + name -> names align even with different method lengths. NO icon.
    cell.mark = t.isFolder ? nil : (t.mark ?: @"");
    cell.text = t.name;
    [cell setNeedsDisplay:YES];
    return cell;
}
// Flip the disclosure triangle when a folder opens/closes (all paths: click, double-click, key, restore).
- (void)refreshDisclosureForItem:(id)item expanded:(BOOL)expanded {
    if (![item isKindOfClass:[TreeItem class]]) return;
    NSInteger row = [_tree rowForItem:item];
    if (row < 0) return;
    NSView *cell = [_tree viewAtColumn:0 row:row makeIfNecessary:NO];
    if ([cell isKindOfClass:[TreeCellView class]]) ((TreeCellView *)cell).isExpanded = expanded;
}
- (void)outlineViewSelectionDidChange:(NSNotification *)note {
    if (_revealingSelection) return;                 // selection from reveal -> do NOT reload (avoid recursion)
    if (_tree.selectedRowIndexes.count != 1) return; // multi-select -> no auto-load
    NSInteger row = _tree.selectedRow;
    if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    if (t.isFolder || t.relPath.length == 0) return;
    [self loadRequestAtRel:t.relPath];
}

// Open/scroll to + select the shown request — open ONLY ancestor branches (O(depth), no
// full-tree scan, no content read; uses id from filename to avoid name collisions).
- (void)revealAndSelectRequestById:(NSString *)reqId relPath:(NSString *)relPath {
    if (relPath.length == 0 || !_tree) return;
    NSArray<NSString *> *parts = [relPath componentsSeparatedByString:@"/"];
    NSArray<TreeItem *> *level = _roots;
    NSString *accum = @"";
    TreeItem *target = nil;
    for (NSUInteger i = 0; i < parts.count; i++) {
        accum = accum.length ? [accum stringByAppendingFormat:@"/%@", parts[i]] : parts[i];
        BOOL isLast = (i + 1 == parts.count);
        TreeItem *match = nil;
        if (isLast) {                                // leaf: prefer EXACT id match, fall back to relPath
            for (TreeItem *t in level)
                if (!t.isFolder && reqId.length && [t.requestId isEqualToString:reqId]) { match = t; break; }
            if (!match)
                for (TreeItem *t in level)
                    if (!t.isFolder && [t.relPath isEqualToString:accum]) { match = t; break; }
            target = match;
        } else {                                     // ancestor folder: find by relPath, unfold lazily
            for (TreeItem *t in level)
                if (t.isFolder && [t.relPath isEqualToString:accum]) { match = t; break; }
            if (!match) return;                      // branch does not exist
            if (!match.childrenLoaded) [self loadChildrenOf:match];  // scan ONLY this folder
            [_tree expandItem:match];
            [_expandedFolders addObject:match.relPath];
            level = match.children;
        }
        if (!match) return;
    }
    if (!target) return;
    NSInteger row = [_tree rowForItem:target];
    if (row < 0) return;
    _revealingSelection = YES;                       // don't trigger a reload
    [_tree selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    [_tree scrollRowToVisible:row];
    _revealingSelection = NO;
}

// Drag-and-drop: move request/folder into a folder.
- (id<NSPasteboardWriting>)outlineView:(NSOutlineView *)ov pasteboardWriterForItem:(id)item {
    TreeItem *t = item;
    if (t.relPath.length == 0) return nil;
    NSPasteboardItem *pb = [[NSPasteboardItem alloc] init];
    [pb setString:t.relPath forType:kTreeDragType];
    return pb;
}
// Session teardown — fires however the drag ends (dropped, cancelled, released outside the window),
// so the indicator can never be left behind.
- (void)outlineView:(NSOutlineView *)ov draggingSession:(NSDraggingSession *)session
       endedAtPoint:(NSPoint)pt operation:(NSDragOperation)op {
    [(DeedOutlineView *)ov hideDropFeedback];
}

// Collect the relPaths being dragged (used to reject dropping a folder into its own subtree).
- (NSArray<NSString *> *)draggedRelPathsFrom:(id<NSDraggingInfo>)info {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (NSPasteboardItem *pb in [[info draggingPasteboard] pasteboardItems]) {
        NSString *src = [pb stringForType:kTreeDragType];
        if (src.length) [out addObject:src];
    }
    return out;
}
// YES if destFolder is the dragged folder itself or lives inside it.
- (BOOL)dropTarget:(NSString *)destFolder isInsideAnyOf:(NSArray<NSString *> *)sources {
    for (NSString *src in sources) {
        if ([destFolder isEqualToString:src]) return YES;
        if ([destFolder hasPrefix:[src stringByAppendingString:@"/"]]) return YES;
    }
    return NO;
}

// Parent folder item of a relPath (nil = root level), among LOADED items.
- (TreeItem *)parentFolderItemOf:(NSString *)rel {
    NSString *parentRel = [rel stringByDeletingLastPathComponent];
    return parentRel.length ? [self loadedFolderItemForRel:parentRel] : nil;
}

// Share of a folder row's height, at each end, that means "insert here" instead of "drop inside".
static const CGFloat kFolderInsertBand = 0.3;

- (NSDragOperation)outlineView:(NSOutlineView *)ov validateDrop:(id<NSDraggingInfo>)info
                   proposedItem:(id)item proposedChildIndex:(NSInteger)idx {
    DeedOutlineView *dov = (DeedOutlineView *)ov;
    TreeItem *t = item;
    // Empty area below the last row: AppKit proposes the outline view itself, which has no row to draw
    // feedback on. Read it as "append at the root level" so the drop shows a bar like any other.
    if (!t && idx == NSOutlineViewDropOnItemIndex) idx = (NSInteger)_roots.count;
    // AppKit proposes the hovered row as the drop PARENT across most of its height, leaving only a
    // hairline between rows to insert at ("first drag does nothing"). Map the row explicitly: a request
    // holds nothing so it splits at the midline; a folder keeps a middle band meaning "drop inside".
    if (t && idx == NSOutlineViewDropOnItemIndex) {
        CGFloat f = [dov rowFractionAtPoint:[ov convertPoint:[info draggingLocation] fromView:nil]];
        if (f < 0) { [dov hideDropFeedback]; return NSDragOperationNone; }
        CGFloat band = t.isFolder ? kFolderInsertBand : 0.5;   // a request has no middle band to keep
        // An EXPANDED folder gives up its lower band: the slot after it sits below its whole subtree,
        // so the bar would land nowhere near the cursor. Its body still means "drop inside".
        BOOL after = f > 1 - band && (!t.isFolder || ![ov isItemExpanded:t]);
        if (f <= band || after) {
            TreeItem *parent = [self parentFolderItemOf:t.relPath];
            NSArray<TreeItem *> *sibs = parent ? parent.children : _roots;
            NSInteger at = [sibs indexOfObject:t];
            if (at == NSNotFound) { [dov hideDropFeedback]; return NSDragOperationNone; }
            item = parent;
            t = parent;
            idx = at + (after ? 1 : 0);
        }
    }
    NSString *destRel = t ? t.relPath : @"";
    if ([self dropTarget:destRel isInsideAnyOf:[self draggedRelPathsFrom:info]]) {
        [dov hideDropFeedback];   // folder into itself / its own subtree
        return NSDragOperationNone;
    }
    if (idx == NSOutlineViewDropOnItemIndex) {      // hovering the folder body -> drop inside it
        [ov setDropItem:item dropChildIndex:NSOutlineViewDropOnItemIndex];
        [dov showDropOnRow:[ov rowForItem:item]];
        return NSDragOperationMove;
    }
    // Between two rows -> reorder at that exact slot.
    [ov setDropItem:item dropChildIndex:idx];
    NSInteger level = item ? [ov levelForItem:item] + 1 : 0;
    NSInteger slotRow = item ? [ov rowForItem:item] + 1 : 0;   // first child row of `item`
    for (NSInteger i = 0; i < idx; ++i) {                       // skip preceding siblings + subtrees
        if (slotRow >= ov.numberOfRows) break;
        id child = [ov itemAtRow:slotRow];
        slotRow += 1 + (child ? [self visibleDescendantCountOf:child in:ov] : 0);
    }
    [dov showDropInsertAtRow:slotRow level:level];
    return NSDragOperationMove;
}

// Rows occupied by an item's expanded descendants (0 when collapsed / not a folder).
- (NSInteger)visibleDescendantCountOf:(id)item in:(NSOutlineView *)ov {
    TreeItem *t = item;
    if (!t.isFolder || ![ov isItemExpanded:t]) return 0;
    NSInteger n = 0;
    for (TreeItem *c in t.children) n += 1 + [self visibleDescendantCountOf:c in:ov];
    return n;
}
// Direct folder children of a level as currently LOADED ("" = root); empty when the level is closed.
- (NSArray<NSString *> *)folderRelsInLoadedLevel:(NSString *)levelRel {
    NSArray<TreeItem *> *items = _roots;
    if (levelRel.length) {
        TreeItem *f = [self loadedFolderItemForRel:levelRel];
        if (!f || !f.childrenLoaded) return @[];
        items = f.children;
    }
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (TreeItem *t in items)
        if (t.isFolder && t.relPath.length) [out addObject:t.relPath];
    return out;
}

// The name after the order-key prefix — the only part of a filename that re-keying leaves alone.
static NSString *TreeRestOfRel(NSString *rel) {
    return N(core::splitOrderPrefix(S([rel lastPathComponent])).rest);
}

// A first drop into a level that had no order keys re-keys every sibling, so folder relPaths change
// under us. Match old to new by that stable part so open folders do not snap shut.
- (void)remapExpandedFolders:(NSArray<NSString *> *)oldRels inLevel:(NSString *)levelRel {
    if (!oldRels.count || !_apiClient) return;
    NSMutableDictionary<NSString *, NSString *> *byRest = [NSMutableDictionary dictionary];
    try {
        for (const auto &n : _apiClient->collection().scanLevel(S(levelRel)))
            if (n.isFolder) byRest[TreeRestOfRel(N(n.relPath))] = N(n.relPath);
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; return; }
    for (NSString *oldRel in oldRels) {
        NSString *fresh = byRest[TreeRestOfRel(oldRel)];
        if (fresh && ![fresh isEqualToString:oldRel]) [self remapExpandedFoldersFrom:oldRel to:fresh];
    }
}

// Display slot of a relPath inside its level; -1 when it is no longer there.
- (NSInteger)levelIndexOfRel:(NSString *)rel inLevel:(NSString *)levelRel {
    if (!_apiClient) return -1;
    NSInteger i = 0;
    try {
        for (const auto &n : _apiClient->collection().scanLevel(S(levelRel))) {
            if ([N(n.relPath) isEqualToString:rel]) return i;
            ++i;
        }
    } catch (const std::exception &) {}
    return -1;
}

- (BOOL)outlineView:(NSOutlineView *)ov acceptDrop:(id<NSDraggingInfo>)info item:(id)item childIndex:(NSInteger)idx {
    [(DeedOutlineView *)ov hideDropFeedback];
    if (!_apiClient) return NO;
    TreeItem *dest = item;
    std::string destFolder = (dest && dest.isFolder) ? std::string(dest.relPath.UTF8String) : std::string();
    BOOL reordering = (idx != NSOutlineViewDropOnItemIndex);
    BOOL any = NO;
    NSString *lastRel = nil;                                   // reselect the drop result afterwards
    NSInteger slot = idx;                                      // insertion slot, advances per item
    NSMutableSet<NSString *> *affected = [NSMutableSet setWithObject:N(destFolder)];  // destination level
    NSArray<NSString *> *foldersBefore = [self folderRelsInLoadedLevel:N(destFolder)];
    for (NSPasteboardItem *pb in [[info draggingPasteboard] pasteboardItems]) {
        NSString *src = [pb stringForType:kTreeDragType];
        if (!src.length) continue;
        NSString *srcParent = [src stringByDeletingLastPathComponent];
        try {
            // Between rows -> reorder (keeps the slot); onto a folder -> plain move, appended at the end.
            std::string newRel = reordering
                                     ? _apiClient->collection().reorder(src.UTF8String, destFolder, (int)slot)
                                     : _apiClient->collection().move(src.UTF8String, destFolder);
            [self remapExpandedFoldersFrom:src to:N(newRel)];  // folder: keep open (no-op for files)
            [affected addObject:srcParent];                    // source level also changes
            lastRel = N(newRel);
            any = YES;
            // The level just changed, so the incoming slot is stale: put the next dragged item right
            // below this one instead of guessing from the pre-drop layout.
            if (reordering) slot = [self levelIndexOfRel:lastRel inLevel:N(destFolder)] + 1;
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    }
    if (any) {
        [self remapExpandedFolders:foldersBefore inLevel:N(destFolder)];
        // A first drop into a never-ordered level keys the whole level, so the OPEN request may have
        // been renamed too — re-resolve it by id before anything writes to the old path.
        [self resyncCurrentRelById];
        for (NSString *p in affected) [self refreshTreeLevel:p];  // only the touched levels
        if (lastRel.length) [self reselectTreeByRel:lastRel];     // reloadData drops the selection
    }
    return any;
}

@end
