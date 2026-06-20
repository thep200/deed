#import "windows/MainWindowController+Private.h"

@implementation MainWindowController (Tree)

#pragma mark Collection / tree

- (void)openFolder:(id)sender {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES; p.canChooseFiles = NO; p.allowsMultipleSelection = NO;
    p.prompt = @"Open Collection";
    if ([p runModal] == NSModalResponseOK) [self openCollectionRoot:p.URL.path];
}

// Giá trị mặc định app-config đọc từ .env (DeedConfig). Dùng khi config.json thiếu key,
// để người dùng chỉnh default qua .env mà không cần sửa code.
- (core::AppConfig)appDefaultsFromEnv {
    DeedConfig *dc = [DeedConfig shared];
    core::AppConfig d;
    d.defaultTimeoutMs = (int)[dc intFor:@"DEFAULT_TIMEOUT_MS" def:30000];
    d.verifyTls = [dc boolFor:@"VERIFY_TLS" def:YES];
    d.fontName = S([dc stringFor:@"FONT_NAME" def:@""]);
    d.fontSize = (int)[dc intFor:@"FONT_SIZE" def:11];
    d.ramCacheSizeMb = (int)[dc intFor:@"RAM_CACHE_SIZE" def:64];
    d.diskCacheSizeMb = (int)[dc intFor:@"DISK_CACHE_SIZE" def:256];
    return d;
}

- (void)openCollectionRoot:(NSString *)path {
    [self autosaveCurrent];
    [_expandedFolders removeAllObjects];   // collection mới: reset trạng thái fold (mặc định fold)
    _root = path.UTF8String;
    core::EngineConfig cfg; cfg.collectionRoot = _root;
    // Trần/sàn cache đọc từ .env (DeedConfig) -> truyền vào Core (Core không tự đọc .env).
    DeedConfig *dc = [DeedConfig shared];
    cfg.cacheLimits.ramMaxMb = (int)[dc intFor:@"RAM_CACHE_SIZE_MAX" def:0];
    cfg.cacheLimits.ramMinMb = (int)[dc intFor:@"RAM_CACHE_SIZE_MIN" def:0];
    cfg.cacheLimits.diskMaxMb = (int)[dc intFor:@"DISK_CACHE_SIZE_MAX" def:0];
    cfg.cacheLimits.diskMinMb = (int)[dc intFor:@"DISK_CACHE_SIZE_MIN" def:0];
    cfg.cacheLimits.thresholdKb = (int)[dc intFor:@"RAM_CACHE_THRESHOLD_KB" def:0];
    cfg.appDefaults = [self appDefaultsFromEnv];   // giá trị mặc định app-config từ .env
    _engine = std::make_unique<core::Engine>(cfg);
    _bridge = std::make_unique<UiDelegateBridge>(self);
    _envVC = [[EnvWindowController alloc] initWithEngine:_engine.get()];
    // Ghi nhớ thư mục này vào app-support để lần sau mở lại đúng nó.
    try { core::AppConfig ac = _engine->appConfig().load(); ac.lastCollectionRoot = _root;
          _engine->appConfig().save(ac); } catch (...) {}
    _openButton.title = [self abbreviatePath:path];
    _openButton.toolTip = path;
    [self setHasRequest:NO];
    // Migrate 1 lần: file cũ -> thêm id vào tên (chỉ đụng file thiếu id). Trước khi dựng cây.
    try { _engine->collection().migrateAddIdToFilenames(); } catch (...) {}
    [self reloadTree];
    [self refreshEnvButton];

    try {
        std::string last = _engine->session().loadLastOpened();
        if (!last.empty()) {
            NSString *full = N(_root + "/" + last);
            if ([[NSFileManager defaultManager] fileExistsAtPath:full]) [self loadRequestAtRel:N(last)];
            else [self toast:[NSString stringWithFormat:@"Not found: %s (skipped)", last.c_str()]];
        }
    } catch (...) {}
}

// Đồng bộ _currentRel theo id ổn định trước khi ghi: sau rename/move, đường dẫn cũ đã đổi
// -> tránh save ghi vào path cũ tạo file "ma". Trả NO nếu request đang mở đã bị xoá.
- (BOOL)resyncCurrentRelById {
    if (_currentId.empty() || !_engine) return !_currentRel.empty();
    std::string rel = _engine->collection().findRelPathById(_currentId);
    if (rel.empty()) return NO;          // không còn trên đĩa (đã xoá) -> đừng tái tạo
    _currentRel = rel;
    return YES;
}

// Nạp con của 1 folder theo yêu cầu (1 lần readdir cấp đó — §3). Không đệ quy.
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
    // T6: reloadData reset selection -> highlight folder/request đang chọn biến mất khi Cmd+S.
    // Lưu relPath đang chọn TRƯỚC reload, khôi phục theo relPath SAU reload (index có thể đổi).
    NSString *selRel = nil;
    NSInteger selRow = _tree.selectedRow;
    if (selRow >= 0) {
        id it = [_tree itemAtRow:selRow];
        if ([it isKindOfClass:[TreeItem class]]) selRel = ((TreeItem *)it).relPath;
    }

    [_roots removeAllObjects];
    if (_engine) {
        try {                               // CHỈ quét cấp gốc; folder con fold mặc định (§3)
            for (const auto &c : _engine->collection().scanLevel(""))
                [_roots addObject:TreeItemFromNode(c)];
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    }
    [_tree reloadData];
    [self restoreExpansion:_roots];         // giữ lại các folder user đang mở qua reload
    if (selRel.length) [self reselectTreeByRel:selRel];
}

// Chọn lại node (folder hoặc request) theo relPath sau reload, KHÔNG kích hoạt auto-load.
- (void)reselectTreeByRel:(NSString *)rel {
    TreeItem *t = [self loadedItemForRel:rel inItems:_roots];
    if (!t) return;
    NSInteger row = [_tree rowForItem:t];
    if (row < 0) return;
    _revealingSelection = YES;
    [_tree selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    _revealingSelection = NO;
}

// Tìm TreeItem (folder hoặc request) theo relPath trong các item ĐÃ NẠP (đệ quy). nil nếu không thấy.
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

// Mở lại các folder đang trong _expandedFolders (lazy: expandItem kích hoạt nạp con).
- (void)restoreExpansion:(NSArray<TreeItem *> *)items {
    for (TreeItem *t in items) {
        if (!t.isFolder) continue;
        if ([_expandedFolders containsObject:t.relPath]) {
            [_tree expandItem:t];           // -> numberOfChildren nạp con nếu cần
            [self restoreExpansion:t.children];  // đệ quy vào con vừa nạp
        }
    }
}

// Tìm TreeItem folder theo relPath TRONG SỐ item ĐÃ NẠP (không đọc đĩa). nil nếu chưa nạp/đóng.
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

// Quét lại 1 cấp (rel; "" = gốc) rồi MERGE vào `items` tại chỗ: giữ lại TreeItem cũ khớp theo
// relPath (bảo toàn con đã nạp + trạng thái mở của nhánh đó), chỉ tạo mới cho entry mới.
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
            // Cập nhật metadata leaf (tên/badge có thể đổi) nhưng GIỮ children + childrenLoaded.
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

// Cập nhật tăng dần cho 1 cấp bị thay đổi (§T1) — KHÔNG đập bỏ toàn cây.
- (void)refreshTreeLevel:(NSString *)parentRel {
    if (!_engine) return;
    if (parentRel.length == 0) {                 // mutation ở GỐC
        [self mergeScanLevel:"" into:_roots];
        [_tree reloadData];
        [self restoreExpansion:_roots];          // expandItem -> KHÔNG re-scan (TreeItem giữ childrenLoaded)
        return;
    }
    TreeItem *f = [self loadedFolderItemForRel:parentRel];
    if (!f) {                                    // folder cha chưa nạp (đang đóng) -> lazy sẽ nạp khi mở
        return;                                  // không cần làm gì: lần expand sau scanLevel mới
    }
    if (f.childrenLoaded) [self mergeScanLevel:f.relPath.UTF8String into:f.children];
    [_tree reloadItem:f reloadChildren:YES];     // chỉ re-query CON của f, nhánh khác nguyên vẹn
}

// Đổi prefix relPath cũ->mới trong _expandedFolders (rename/move folder) — giữ trạng thái mở (§T3).
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
// Click vào folder -> fold/unfold.
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

// Rename qua dialog Platinum (CUSTOM_DIALOG §6.1): prompt + validate, rồi đồng bộ tên file (LAZY_TREE §4).
- (void)promptRenameItem:(TreeItem *)t {
    if (!t || t.relPath.length == 0 || !_engine) return;
    NSString *newName = [OS9Dialog promptWithTitle:@"Rename"
                                           message:@"New name:"
                                       defaultText:(t.name ?: @"")
                                       placeholder:@"Name"
                                          okButton:@"Rename"
                                      cancelButton:@"Cancel"
                                          validate:^NSString *(NSString *s) {
        NSString *tr = [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (tr.length == 0) return @"Name cannot be empty";
        return nil;
    }
                                            parent:_window];
    if (!newName || [newName isEqualToString:t.name]) return;   // cancel hoặc không đổi
    [self autosaveCurrent];
    BOOL wasCurrent = (!_currentId.empty() && t.requestId.length && S(t.requestId) == _currentId);
    NSString *oldRel = t.relPath;
    NSString *parentRel = [oldRel stringByDeletingLastPathComponent];
    try {
        std::string newRel = _engine->collection().rename(oldRel.UTF8String, newName.UTF8String);
        if (t.isFolder) [self remapExpandedFoldersFrom:oldRel to:N(newRel)];  // §T3: giữ trạng thái mở
        if (wasCurrent) {
            // Đồng bộ tên vào model đang mở: nếu không, Save sau đó ghi tên CŨ -> rollback tên file.
            _model.name = newName.UTF8String;
            _currentRel = newRel;
            [self updateTitle];
        }
        [self refreshTreeLevel:parentRel];   // §T1: chỉ quét lại cấp chứa item, không re-scan toàn cây
        if (wasCurrent) [self revealAndSelectRequestById:N(_currentId) relPath:N(_currentRel)];
        [self toastOk:@"Renamed"];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

- (void)outlineViewItemDidExpand:(NSNotification *)n {
    TreeItem *t = n.userInfo[@"NSObject"];
    if (t.relPath) [_expandedFolders addObject:t.relPath];
    [self refreshDisclosureForItem:t expanded:YES];   // lật tam giác ▷ -> ▽
}
- (void)outlineViewItemDidCollapse:(NSNotification *)n {
    TreeItem *t = n.userInfo[@"NSObject"];
    if (t.relPath) [_expandedFolders removeObject:t.relPath];
    [self refreshDisclosureForItem:t expanded:NO];     // lật tam giác ▽ -> ▷
}

- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    if (item == nil) return _roots.count;
    TreeItem *t = item;
    if (!t.isFolder) return 0;
    if (!t.childrenLoaded) [self loadChildrenOf:t];   // lazy: chỉ nạp khi cần đếm/hiển thị
    return t.children.count;
}
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)idx ofItem:(id)item {
    if (item == nil) return _roots[idx];
    TreeItem *t = item;
    if (!t.childrenLoaded) [self loadChildrenOf:t];
    return t.children[idx];
}
// Folder LUÔN expandable (rẻ, không đọc đĩa); request: không.
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item { return ((TreeItem *)item).isFolder; }
// VIỆC 3: row view tự vẽ nền xám khi selected (ảo hoá/tái dùng của NSOutlineView).
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
    cell.isExpanded = t.isFolder && [ov isItemExpanded:item];   // tam giác ▽/▷
    // Request: method ở cột riêng (mark) + tên -> tên thẳng hàng dù method dài/ngắn khác nhau. KHÔNG icon.
    cell.mark = t.isFolder ? nil : (t.mark ?: @"");
    cell.text = t.name;
    [cell setNeedsDisplay:YES];
    return cell;
}
// Lật tam giác disclosure khi folder mở/đóng (mọi đường: click, double-click, phím, restore).
- (void)refreshDisclosureForItem:(id)item expanded:(BOOL)expanded {
    if (![item isKindOfClass:[TreeItem class]]) return;
    NSInteger row = [_tree rowForItem:item];
    if (row < 0) return;
    NSView *cell = [_tree viewAtColumn:0 row:row makeIfNecessary:NO];
    if ([cell isKindOfClass:[TreeCellView class]]) ((TreeCellView *)cell).isExpanded = expanded;
}
- (void)outlineViewSelectionDidChange:(NSNotification *)note {
    if (_revealingSelection) return;                 // chọn do reveal -> KHÔNG nạp lại (tránh đệ quy)
    if (_tree.selectedRowIndexes.count != 1) return; // multi-select -> không auto-load
    NSInteger row = _tree.selectedRow;
    if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    if (t.isFolder || t.relPath.length == 0) return;
    [self loadRequestAtRel:t.relPath];
}

// VIỆC 2B: mở/cuộn tới + chọn request đang hiển thị — CHỈ mở nhánh tổ tiên (O(depth), không scan
// toàn cây, không đọc nội dung; dùng id từ tên file để chống trùng tên).
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
        if (isLast) {                                // lá: ưu tiên khớp id EXACT, fallback relPath
            for (TreeItem *t in level)
                if (!t.isFolder && reqId.length && [t.requestId isEqualToString:reqId]) { match = t; break; }
            if (!match)
                for (TreeItem *t in level)
                    if (!t.isFolder && [t.relPath isEqualToString:accum]) { match = t; break; }
            target = match;
        } else {                                     // folder tổ tiên: tìm theo relPath, unfold lazy
            for (TreeItem *t in level)
                if (t.isFolder && [t.relPath isEqualToString:accum]) { match = t; break; }
            if (!match) return;                      // nhánh không tồn tại
            if (!match.childrenLoaded) [self loadChildrenOf:match];  // CHỈ scan folder này
            [_tree expandItem:match];
            [_expandedFolders addObject:match.relPath];
            level = match.children;
        }
        if (!match) return;
    }
    if (!target) return;
    NSInteger row = [_tree rowForItem:target];
    if (row < 0) return;
    _revealingSelection = YES;                       // không kích hoạt nạp lại
    [_tree selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    [_tree scrollRowToVisible:row];
    _revealingSelection = NO;
}

// --- Kéo-thả: di chuyển request/folder vào folder ---
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
    if (item == nil || t.isFolder) {            // chỉ thả vào folder hoặc gốc
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
    NSMutableSet<NSString *> *affected = [NSMutableSet setWithObject:N(destFolder)];  // cấp đích
    for (NSPasteboardItem *pb in [[info draggingPasteboard] pasteboardItems]) {
        NSString *src = [pb stringForType:kTreeDragType];
        if (!src.length) continue;
        NSString *srcParent = [src stringByDeletingLastPathComponent];
        try {
            std::string newRel = _engine->collection().move(src.UTF8String, destFolder);
            [self remapExpandedFoldersFrom:src to:N(newRel)];  // folder: giữ mở (no-op cho file)
            [affected addObject:srcParent];                    // cấp nguồn cũng đổi
            any = YES;
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    }
    if (any) for (NSString *p in affected) [self refreshTreeLevel:p];  // §T1: chỉ các cấp đụng tới
    return any;
}

#pragma mark Tree context menu (chuột phải) + multi-select

- (NSMenu *)contextMenuForRow:(NSInteger)row {
    if (!_engine) return nil;
    // Nếu chuột phải vào item chưa được chọn -> chọn riêng item đó.
    if (row >= 0 && ![_tree.selectedRowIndexes containsIndex:row])
        [_tree selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    if (row < 0) [_tree deselectAll:nil]; // vùng trống -> thao tác ở gốc cây

    NSMenu *m = [[NSMenu alloc] init];
    NSUInteger selCount = _tree.selectedRowIndexes.count;

    // Chọn nhiều -> chỉ Delete.
    if (selCount > 1) {
        [[m addItemWithTitle:[NSString stringWithFormat:@"Delete %lu items", (unsigned long)selCount]
                      action:@selector(deleteSelectedMulti:) keyEquivalent:@""] setTarget:self];
        return m;
    }

    TreeItem *t = (row >= 0) ? [_tree itemAtRow:row] : nil;
    if (t == nil || t.isFolder) {
        // Vùng trống hoặc folder -> thêm request/folder.
        [[m addItemWithTitle:@"New HTTP Request" action:@selector(newHttp:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:@"New gRPC Request" action:@selector(newGrpc:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:@"New Folder" action:@selector(newFolder:) keyEquivalent:@""] setTarget:self];
        if (t != nil) { // folder cũng cho rename/dup/delete
            [m addItem:[NSMenuItem separatorItem]];
            [[m addItemWithTitle:@"Rename" action:@selector(renameSel:) keyEquivalent:@""] setTarget:self];
            [[m addItemWithTitle:@"Duplicate" action:@selector(dupSel:) keyEquivalent:@""] setTarget:self];
            [[m addItemWithTitle:@"Delete" action:@selector(deleteSel:) keyEquivalent:@""] setTarget:self];
        }
    } else {
        // Request -> rename / duplicate / delete.
        [[m addItemWithTitle:@"Rename" action:@selector(renameSel:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:@"Duplicate" action:@selector(dupSel:) keyEquivalent:@""] setTarget:self];
        [[m addItemWithTitle:@"Delete" action:@selector(deleteSel:) keyEquivalent:@""] setTarget:self];
    }
    return m;
}

// Xoá cache response của request bị xoá (cả 2 tầng — §7). §T2: id LẤY TỪ TÊN FILE (zero-read)
// qua parseRequestFilename/scanLevel; CHỈ fallback đọc (và có thể ghi) nội dung cho file legacy
// thiếu id trong tên -> không đọc/ghi cả folder ngay trước khi xoá.
- (void)purgeCacheAtRel:(NSString *)rel isFolder:(BOOL)isFolder {
    if (!_engine || rel.length == 0) return;
    if (!isFolder) {
        core::ParsedRequestName p = core::parseRequestFilename(rel.lastPathComponent.UTF8String);
        std::string id = p.id;
        if (id.empty()) {                         // file legacy chưa có id trong tên -> đọc nội dung
            try { id = _engine->collection().loadRequest(rel.UTF8String).id; } catch (...) {}
        }
        if (!id.empty()) _engine->removeResponse(id);
        return;
    }
    try {
        for (const auto &c : _engine->collection().scanLevel(rel.UTF8String)) {
            if (c.isFolder) [self purgeCacheAtRel:N(c.relPath) isFolder:YES];
            else if (!c.id.empty()) _engine->removeResponse(c.id);  // id sẵn từ scanLevel (zero-read)
            else [self purgeCacheAtRel:N(c.relPath) isFolder:NO];   // legacy -> nhánh đọc nội dung
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
    NSInteger r = [OS9Dialog confirmWithTitle:@"Delete"
                                      message:[NSString stringWithFormat:@"Delete %lu selected items? This cannot be undone.",
                                               (unsigned long)items.count]
                                      buttons:@[ @"Cancel", @"Delete" ]
                                defaultButton:1 cancelButton:0 icon:OS9AlertCaution parent:_window];
    if (r != 1) return;
    [self closeEditorIfDeleted:items];    // tránh autosave tạo lại file vừa xoá
    NSMutableSet<NSString *> *parents = [NSMutableSet set];
    for (TreeItem *t in items) [parents addObject:[t.relPath stringByDeletingLastPathComponent]];
    for (TreeItem *t in items) [self purgeCacheAtRel:t.relPath isFolder:t.isFolder];
    for (TreeItem *t in items) { try { _engine->collection().remove(t.relPath.UTF8String); } catch (...) {} }
    for (NSString *p in parents) [self refreshTreeLevel:p];   // §T1: chỉ các cấp cha bị xoá
}
- (std::string)selectedFolderRel {
    NSInteger row = _tree.selectedRow;
    if (row < 0) return "";
    TreeItem *t = [_tree itemAtRow:row];
    if (t.isFolder) return t.relPath.UTF8String;
    return [t.relPath stringByDeletingLastPathComponent].UTF8String;
}
- (void)newHttp:(id)s { [self createRequest:core::RequestType::Http name:@"New Request"]; }
- (void)newGrpc:(id)s { [self createRequest:core::RequestType::Grpc name:@"New RPC"]; }
// Tên mặc định, KHÔNG popup. Đổi tên sau bằng inline-rename trên cây. loadRequestAtRel
// tự autosave request đang mở trước khi chuyển.
- (void)createRequest:(core::RequestType)t name:(NSString *)name {
    if (!_engine) { [self toastWarn:@"Open a collection folder first"]; return; }
    try {
        std::string folderRel = [self selectedFolderRel];
        std::string rel = _engine->collection().createRequest(folderRel, t, name.UTF8String);
        [self refreshTreeLevel:N(folderRel)];   // §T1: chỉ quét lại folder đích (reveal sẽ mở nếu đang đóng)
        [self loadRequestAtRel:N(rel)];
        [self toastOk:[NSString stringWithFormat:@"Created: %@", name]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)newFolder:(id)s {
    if (!_engine) { [self toastWarn:@"Open a collection folder first"]; return; }
    try {
        std::string folderRel = [self selectedFolderRel];
        _engine->collection().createFolder(folderRel, "New Folder");
        [self refreshTreeLevel:N(folderRel)];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
// Rename: chỉnh ngay trên cây (inline), không popup.
- (void)renameSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    [self promptRenameItem:[_tree itemAtRow:row]];
}
- (void)dupSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    [self autosaveCurrent];   // flush edits hiện tại trước (tránh trạng thái treo)
    NSString *parentRel = [t.relPath stringByDeletingLastPathComponent];
    try {
        std::string dupRel = _engine->collection().duplicate(t.relPath.UTF8String);
        [self refreshTreeLevel:parentRel];   // §T1: bản sao nằm cùng cấp -> chỉ quét lại cấp đó
        if (!t.isFolder) [self loadRequestAtRel:N(dupRel)];   // mở bản sao -> _currentRel đúng
        [self toastOk:@"Duplicated"];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)deleteSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    NSInteger r = [OS9Dialog confirmWithTitle:@"Delete"
                                      message:[NSString stringWithFormat:@"Delete \"%@\"? This cannot be undone.", t.name]
                                      buttons:@[ @"Cancel", @"Delete" ]
                                defaultButton:1 cancelButton:0 icon:OS9AlertCaution parent:_window];
    if (r != 1) return;
    [self closeEditorIfDeleted:@[ t ]];   // tránh autosave tạo lại file vừa xoá
    [self purgeCacheAtRel:t.relPath isFolder:t.isFolder];
    NSString *parentRel = [t.relPath stringByDeletingLastPathComponent];
    try { _engine->collection().remove(t.relPath.UTF8String); [self refreshTreeLevel:parentRel]; }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

@end
