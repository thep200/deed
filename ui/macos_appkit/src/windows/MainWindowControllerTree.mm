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
    d.fontSize = (int)[dc intFor:@"FONT_SIZE" def:11];
    d.ramCacheSizeMb = (int)[dc intFor:@"RAM_CACHE_SIZE" def:64];
    d.diskCacheSizeMb = (int)[dc intFor:@"DISK_CACHE_SIZE" def:256];
    return d;
}

- (void)openCollectionRoot:(NSString *)path {
    [self autosaveCurrent];
    [_expandedFolders removeAllObjects];   // new collection: reset fold state (folded by default)
    _root = path.UTF8String;
    core::EngineConfig cfg; cfg.collectionRoot = _root;
    // Cache ceiling/floor read from .env (DeedConfig) -> passed into Core (Core doesn't read .env).
    DeedConfig *dc = [DeedConfig shared];
    cfg.cacheLimits.ramMaxMb = (int)[dc intFor:@"RAM_CACHE_SIZE_MAX" def:0];
    cfg.cacheLimits.ramMinMb = (int)[dc intFor:@"RAM_CACHE_SIZE_MIN" def:0];
    cfg.cacheLimits.diskMaxMb = (int)[dc intFor:@"DISK_CACHE_SIZE_MAX" def:0];
    cfg.cacheLimits.diskMinMb = (int)[dc intFor:@"DISK_CACHE_SIZE_MIN" def:0];
    cfg.cacheLimits.thresholdKb = (int)[dc intFor:@"RAM_CACHE_THRESHOLD_KB" def:0];
    // Stream ceilings from .env (SPEC_grpc_streaming §9; 0 -> sender default). MiB -> bytes for max bytes.
    cfg.streamLimits.maxEvents = (uint64_t)[dc intFor:@"STREAM_MAX_EVENTS" def:0];
    cfg.streamLimits.maxBytes = (uint64_t)[dc intFor:@"STREAM_MAX_BYTES_MB" def:0] * 1024ull * 1024ull;
    // WebSocket tunables from .env (SPEC_websocket §9; 0 -> WsSender default).
    cfg.wsLimits.pingIntervalMs = (int)[dc intFor:@"WS_PING_INTERVAL_MS" def:0];
    cfg.wsLimits.idleTimeoutMs = (int)[dc intFor:@"WS_IDLE_TIMEOUT_MS" def:0];
    cfg.wsLimits.closeTimeoutMs = (int)[dc intFor:@"WS_CLOSE_TIMEOUT_MS" def:0];
    cfg.wsLimits.maxFrameBytes = (int)[dc intFor:@"WS_MAX_FRAME_MB" def:0] * 1024 * 1024;
    cfg.wsLimits.sendQueueMaxFrames = (int)[dc intFor:@"WS_SEND_QUEUE_MAX_FRAMES" def:0];
    cfg.wsLimits.sendQueueMaxBytes = (int)[dc intFor:@"WS_SEND_QUEUE_MAX_MB" def:0] * 1024 * 1024;
    cfg.appDefaults = [self appDefaultsFromEnv];   // app-config defaults from .env
    _engine = std::make_unique<core::Engine>(cfg);
    // coalesce cadence + UI buffer high-water mark (backpressure valve, perf spec §2.2/§10).
    _bridge = std::make_shared<UiDelegateBridge>(self,
                                                 (int)[dc intFor:@"STREAM_COALESCE_MS" def:50],
                                                 (int)[dc intFor:@"STREAM_UI_BUFFER_MAX_KB" def:8192]);
    _envVC = [[EnvWindowController alloc] initWithEngine:_engine.get()];
    // Remember this folder in app-support so it reopens next time.
    try { core::AppConfig ac = _engine->appConfig().load(); ac.lastCollectionRoot = _root;
          _engine->appConfig().save(ac); } catch (...) {}
    _openButton.title = [self abbreviatePath:path];
    _openButton.toolTip = path;
    [self setHasRequest:NO];
    // One-time migrate: old files -> add id to filename (only touches files missing id). Before building tree.
    try { _engine->collection().migrateAddIdToFilenames(); } catch (...) {}
    [self reloadTree];
    [self refreshEnvButton];

    try {
        std::string last = _engine->session().loadLastOpened();
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
    if (_currentId.empty() || !_engine) return !_currentRel.empty();
    std::string rel = _engine->collection().findRelPathById(_currentId);
    if (rel.empty()) return NO;          // no longer on disk (deleted) -> don't recreate
    _currentRel = rel;
    return YES;
}

// Load a folder's children on demand (one readdir of that level — §3). No recursion.
// M13: this runs synchronously on the main thread, but it scans ONLY the expanded level (lazy, never the
// whole tree), so the cost is bounded to one directory's entries — acceptable for the expected collection
// size. If a single folder is ever expected to hold thousands of requests, move this scan to a background
// queue with a placeholder row + async reload (a larger rearchitecture).
- (void)loadChildrenOf:(TreeItem *)folder {
    if (!folder || folder.childrenLoaded || !_engine) return;
    [folder.children removeAllObjects];
    try {
        for (const auto &c : _engine->collection().scanLevel(folder.relPath.UTF8String))
            [folder.children addObject:TreeItemFromNode(c)];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    folder.childrenLoaded = YES;
}

- (void)reloadTree {
    // T6: reloadData resets selection -> the selected folder/request highlight vanishes on Cmd+S.
    // Save selected relPath BEFORE reload, restore by relPath AFTER reload (index may change).
    NSString *selRel = nil;
    NSInteger selRow = _tree.selectedRow;
    if (selRow >= 0) {
        id it = [_tree itemAtRow:selRow];
        if ([it isKindOfClass:[TreeItem class]]) selRel = ((TreeItem *)it).relPath;
    }

    [_roots removeAllObjects];
    if (_engine) {
        try {                               // scan ROOT level only; child folders folded by default (§3)
            for (const auto &c : _engine->collection().scanLevel(""))
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
- (void)mergeScanLevel:(const std::string &)rel into:(NSMutableArray<TreeItem *> *)items {
    if (!_engine) return;
    std::vector<core::TreeNode> nodes;
    try { nodes = _engine->collection().scanLevel(rel); }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; return; }
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
}

// Incremental update for one changed level (§T1) — does NOT tear down the whole tree.
- (void)refreshTreeLevel:(NSString *)parentRel {
    if (!_engine) return;
    if (parentRel.length == 0) {                 // mutation at ROOT
        [self mergeScanLevel:"" into:_roots];
        [_tree reloadData];
        [self restoreExpansion:_roots];          // expandItem -> NO re-scan (TreeItem keeps childrenLoaded)
        return;
    }
    TreeItem *f = [self loadedFolderItemForRel:parentRel];
    if (!f) {                                    // parent folder not loaded (closed) -> lazy load on open
        return;                                  // nothing to do: next expand will scanLevel
    }
    if (f.childrenLoaded) [self mergeScanLevel:f.relPath.UTF8String into:f.children];
    [_tree reloadItem:f reloadChildren:YES];     // re-query only f's CHILDREN, other branches intact
}

// Remap old->new relPath prefix in _expandedFolders (folder rename/move) — keep open state (§T3).
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
    if (row < 0) { [self newHttp:nil]; return; }   // empty area
    [self promptRenameItem:[_tree itemAtRow:row]];
}

// Rename via Platinum dialog (CUSTOM_DIALOG §6.1): prompt + validate, then sync the filename (LAZY_TREE §4).
- (void)promptRenameItem:(TreeItem *)t {
    if (!t || t.relPath.length == 0 || !_engine) return;
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
        std::string newRel = _engine->collection().rename(oldRel.UTF8String, newName.UTF8String);
        if (t.isFolder) [self remapExpandedFoldersFrom:oldRel to:N(newRel)];  // §T3: keep open state
        if (wasCurrent) {
            // Sync the name into the open model: otherwise a later Save writes the OLD name -> filename rollback.
            _model.name = newName.UTF8String;
            _currentRel = newRel;
            [self updateTitle];
        }
        [self refreshTreeLevel:parentRel];   // §T1: rescan only the level holding the item, not the whole tree
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
// TASK 3: row view self-draws gray background when selected (NSOutlineView virtualization/reuse).
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

// TASK 2B: open/scroll to + select the shown request — open ONLY ancestor branches (O(depth), no
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

// --- Drag-and-drop: move request/folder into a folder ---
- (id<NSPasteboardWriting>)outlineView:(NSOutlineView *)ov pasteboardWriterForItem:(id)item {
    TreeItem *t = item;
    if (t.relPath.length == 0) return nil;
    NSPasteboardItem *pb = [[NSPasteboardItem alloc] init];
    [pb setString:t.relPath forType:kTreeDragType];
    return pb;
}
- (NSDragOperation)outlineView:(NSOutlineView *)ov validateDrop:(id<NSDraggingInfo>)info
                   proposedItem:(id)item proposedChildIndex:(NSInteger)idx {
    TreeItem *t = item;
    if (item == nil || t.isFolder) {            // only drop onto a folder or root
        [ov setDropItem:item dropChildIndex:NSOutlineViewDropOnItemIndex];
        return NSDragOperationMove;
    }
    return NSDragOperationNone;
}
- (BOOL)outlineView:(NSOutlineView *)ov acceptDrop:(id<NSDraggingInfo>)info item:(id)item childIndex:(NSInteger)idx {
    if (!_engine) return NO;
    TreeItem *dest = item;
    std::string destFolder = (dest && dest.isFolder) ? std::string(dest.relPath.UTF8String) : std::string();
    BOOL any = NO;
    NSMutableSet<NSString *> *affected = [NSMutableSet setWithObject:N(destFolder)];  // destination level
    for (NSPasteboardItem *pb in [[info draggingPasteboard] pasteboardItems]) {
        NSString *src = [pb stringForType:kTreeDragType];
        if (!src.length) continue;
        NSString *srcParent = [src stringByDeletingLastPathComponent];
        try {
            std::string newRel = _engine->collection().move(src.UTF8String, destFolder);
            [self remapExpandedFoldersFrom:src to:N(newRel)];  // folder: keep open (no-op for files)
            [affected addObject:srcParent];                    // source level also changes
            any = YES;
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    }
    if (any) for (NSString *p in affected) [self refreshTreeLevel:p];  // §T1: only the touched levels
    return any;
}

#pragma mark Tree context menu (right-click) + multi-select

- (NSMenu *)contextMenuForRow:(NSInteger)row {
    if (!_engine) return nil;
    // Right-click on an unselected item -> select just that item.
    if (row >= 0 && ![_tree.selectedRowIndexes containsIndex:row])
        [_tree selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    if (row < 0) [_tree deselectAll:nil]; // empty area -> operate at tree root

    NSMenu *m = [[NSMenu alloc] init];
    NSUInteger selCount = _tree.selectedRowIndexes.count;

    // Multi-select -> Delete only.
    if (selCount > 1) {
        [[m addItemWithTitle:[NSString stringWithFormat:StrFmtDeleteItems, (unsigned long)selCount]
                      action:@selector(deleteSelectedMulti:) keyEquivalent:@""] setTarget:self];
        return m;
    }

    TreeItem *t = (row >= 0) ? [_tree itemAtRow:row] : nil;
    if (t == nil || t.isFolder) {
        // Empty area or folder -> add request/folder.
        [[m addItemWithTitle:StrMenuNewHttp action:@selector(newHttp:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:StrMenuNewGrpc action:@selector(newGrpc:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:StrMenuNewWs action:@selector(newWs:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:StrMenuNewGraphQl action:@selector(newGraphQl:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:StrNewFolder action:@selector(newFolder:) keyEquivalent:@""] setTarget:self];
        if (t != nil) { // folder also allows rename/dup/delete
            [m addItem:[NSMenuItem separatorItem]];
            [[m addItemWithTitle:StrRename action:@selector(renameSel:) keyEquivalent:@""] setTarget:self];
            [[m addItemWithTitle:StrDuplicate action:@selector(dupSel:) keyEquivalent:@""] setTarget:self];
            [[m addItemWithTitle:StrDelete action:@selector(deleteSel:) keyEquivalent:@""] setTarget:self];
        }
    } else {
        // Request -> rename / duplicate / delete.
        [[m addItemWithTitle:StrRename action:@selector(renameSel:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:StrDuplicate action:@selector(dupSel:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:StrDelete action:@selector(deleteSel:) keyEquivalent:@""] setTarget:self];
    }
    return m;
}

// Purge the response cache of a deleted request (both tiers — §7). §T2: id TAKEN FROM FILENAME (zero-read)
// via parseRequestFilename/scanLevel; ONLY fall back to reading (and possibly writing) content for
// legacy files missing the id in their name -> avoid reading/writing the whole folder right before delete.
- (void)purgeCacheAtRel:(NSString *)rel isFolder:(BOOL)isFolder {
    if (!_engine || rel.length == 0) return;
    if (!isFolder) {
        core::ParsedRequestName p = core::parseRequestFilename(rel.lastPathComponent.UTF8String);
        std::string id = p.id;
        if (id.empty()) {                         // legacy file with no id in name -> read content
            try { id = _engine->collection().loadRequest(rel.UTF8String).id; } catch (...) {}
        }
        if (!id.empty()) _engine->removeResponse(id);
        return;
    }
    try {
        for (const auto &c : _engine->collection().scanLevel(rel.UTF8String)) {
            if (c.isFolder) [self purgeCacheAtRel:N(c.relPath) isFolder:YES];
            else if (!c.id.empty()) _engine->removeResponse(c.id);  // id already from scanLevel (zero-read)
            else [self purgeCacheAtRel:N(c.relPath) isFolder:NO];   // legacy -> content-reading branch
        }
    } catch (...) {}
}

- (void)deleteSelectedMulti:(id)sender {
    NSMutableArray<TreeItem *> *items = [NSMutableArray array];
    [_tree.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger idx, BOOL *stop) {
        TreeItem *t = [_tree itemAtRow:idx];
        if (t.relPath.length) [items addObject:t];
    }];
    if (items.count == 0) return;
    NSInteger r = [OS9Dialog confirmWithTitle:StrDelete
                                      message:[NSString stringWithFormat:StrFmtConfirmDeleteMulti,
                                               (unsigned long)items.count]
                                      buttons:@[ StrCancel, StrDelete ]
                                defaultButton:1 cancelButton:0 icon:OS9AlertNone parent:_window];
    if (r != 1) return;
    [self closeEditorIfDeleted:items];    // prevent autosave from recreating the just-deleted file
    NSMutableSet<NSString *> *parents = [NSMutableSet set];
    for (TreeItem *t in items) [parents addObject:[t.relPath stringByDeletingLastPathComponent]];
    for (TreeItem *t in items) [self purgeCacheAtRel:t.relPath isFolder:t.isFolder];
    for (TreeItem *t in items) { try { _engine->collection().remove(t.relPath.UTF8String); } catch (...) {} }
    for (NSString *p in parents) [self refreshTreeLevel:p];   // §T1: only the affected parent levels
}
- (std::string)selectedFolderRel {
    NSInteger row = _tree.selectedRow;
    if (row < 0) return "";
    TreeItem *t = [_tree itemAtRow:row];
    if (t.isFolder) return t.relPath.UTF8String;
    return [t.relPath stringByDeletingLastPathComponent].UTF8String;
}
- (void)newHttp:(id)s { [self createRequest:core::RequestType::Http name:StrDefaultRequestName]; }
- (void)newGrpc:(id)s { [self createRequest:core::RequestType::Grpc name:StrDefaultRpcName]; }
- (void)newWs:(id)s { [self createRequest:core::RequestType::WebSocket name:StrDefaultWsName]; }
- (void)newGraphQl:(id)s { [self createRequest:core::RequestType::GraphQL name:StrDefaultGqlName]; }
// Default name, NO popup. Rename later via inline-rename in the tree. loadRequestAtRel
// autosaves the open request before switching.
- (void)createRequest:(core::RequestType)t name:(NSString *)name {
    if (!_engine) { [self toastWarn:StrToastOpenFolderFirst]; return; }
    try {
        std::string folderRel = [self selectedFolderRel];
        std::string rel = _engine->collection().createRequest(folderRel, t, name.UTF8String);
        [self refreshTreeLevel:N(folderRel)];   // §T1: rescan only the target folder (reveal opens it if closed)
        [self loadRequestAtRel:N(rel)];
        [self toastOk:[NSString stringWithFormat:StrFmtToastCreated, name]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)newFolder:(id)s {
    if (!_engine) { [self toastWarn:StrToastOpenFolderFirst]; return; }
    try {
        std::string folderRel = [self selectedFolderRel];
        _engine->collection().createFolder(folderRel, StrNewFolder.UTF8String);
        [self refreshTreeLevel:N(folderRel)];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
// Rename: edit inline in the tree, no popup.
- (void)renameSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    [self promptRenameItem:[_tree itemAtRow:row]];
}
- (void)dupSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    [self autosaveCurrent];   // flush current edits first (avoid dangling state)
    NSString *parentRel = [t.relPath stringByDeletingLastPathComponent];
    try {
        std::string dupRel = _engine->collection().duplicate(t.relPath.UTF8String);
        [self refreshTreeLevel:parentRel];   // §T1: the copy is at the same level -> rescan only that level
        if (!t.isFolder) [self loadRequestAtRel:N(dupRel)];   // open the copy -> correct _currentRel
        [self toastOk:StrToastDuplicated];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)deleteSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    NSInteger r = [OS9Dialog confirmWithTitle:StrDelete
                                      message:[NSString stringWithFormat:StrFmtConfirmDelete, t.name]
                                      buttons:@[ StrCancel, StrDelete ]
                                defaultButton:1 cancelButton:0 icon:OS9AlertNone parent:_window];
    if (r != 1) return;
    [self closeEditorIfDeleted:@[ t ]];   // prevent autosave from recreating the just-deleted file
    [self purgeCacheAtRel:t.relPath isFolder:t.isFolder];
    NSString *parentRel = [t.relPath stringByDeletingLastPathComponent];
    try { _engine->collection().remove(t.relPath.UTF8String); [self refreshTreeLevel:parentRel]; }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

@end
