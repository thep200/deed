#import "windows/MainWindowControllerPrivate.h"

@implementation MainWindowController (TreeMenu)

#pragma mark Tree context menu (right-click) + multi-select

- (void)showContextMenuForRow:(NSInteger)row atWindowPoint:(NSPoint)pt {
    if (!_apiClient) return;
    // Right-click on an unselected item -> select just that item.
    if (row >= 0 && ![_tree.selectedRowIndexes containsIndex:row])
        [_tree selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    if (row < 0) [_tree deselectAll:nil]; // empty area -> operate at tree root

    NSMutableArray<OS9MenuEntry *> *items = [NSMutableArray array];
    NSUInteger selCount = _tree.selectedRowIndexes.count;
    __weak MainWindowController *ws = self;

    // Multi-select -> Delete only.
    if (selCount > 1) {
        NSString *title = [NSString stringWithFormat:StrFmtDeleteItems, (unsigned long)selCount];
        [items addObject:[OS9MenuEntry entry:title action:^{ [ws deleteSelectedMulti:nil]; }]];
        OS9ShowContextMenu(items, _tree, pt);
        return;
    }

    TreeItem *t = (row >= 0) ? [_tree itemAtRow:row] : nil;
    if (t == nil || t.isFolder) {
        // Empty area or folder -> one "New <type>" item per binder, in display order.
        for (RequestTypeUi *ui in TypeUisInMenuOrder()) {
            core::RequestType tt = ui.type;
            NSString *name = ui.defaultRequestName;
            [items addObject:[OS9MenuEntry entry:ui.newMenuTitle
                                          action:^{ [ws createRequest:tt name:name]; }]];
        }
        [items addObject:[OS9MenuEntry entry:StrNewFolder action:^{ [ws newFolder:nil]; }]];
        if (t != nil) { // folder also allows rename/dup/delete
            [items addObject:[OS9MenuEntry separator]];
            [items addObject:[OS9MenuEntry entry:StrRename    action:^{ [ws renameSel:nil]; }]];
            [items addObject:[OS9MenuEntry entry:StrDuplicate action:^{ [ws dupSel:nil]; }]];
            [items addObject:[OS9MenuEntry entry:StrDelete    action:^{ [ws deleteSel:nil]; }]];
        }
    } else {
        // Request -> (Copy-as-cURL where the binder offers it) rename / duplicate / delete.
        if (TypeUiFor(t.requestType).offersCurlMenu) {
            NSString *rel = t.relPath;   // copy THIS request (not whatever is open in the editor)
            [items addObject:[OS9MenuEntry entry:StrMenuCopyCurl action:^{ [ws copyCurlForRel:rel]; }]];
            [items addObject:[OS9MenuEntry separator]];
        }
        [items addObject:[OS9MenuEntry entry:StrRename    action:^{ [ws renameSel:nil]; }]];
        [items addObject:[OS9MenuEntry entry:StrDuplicate action:^{ [ws dupSel:nil]; }]];
        [items addObject:[OS9MenuEntry entry:StrDelete    action:^{ [ws deleteSel:nil]; }]];
    }
    OS9ShowContextMenu(items, _tree, pt);
}

// Copy a tree request as cURL/grpcurl (right-click). Copies the RIGHT-CLICKED request, not
// necessarily the one open in the editor. If it IS the open request, defer to copyAsCurl: so any
// unsaved editor edits are included.
- (void)copyCurlForRel:(NSString *)rel {
    if (!_apiClient || rel.length == 0) return;
    if (_hasRequest && _currentRel == S(rel)) { [self copyAsCurl:nil]; return; }
    try {
        core::domain::RequestModel m = _apiClient->collection().loadRequest(rel.UTF8String);
        std::string curl = _apiClient->exportCurl(m);
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:N(curl) forType:NSPasteboardTypeString];
        [self toastOk:StrToastCopiedCurl];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// Purge the response cache of a deleted request (both tiers). Id TAKEN FROM FILENAME (zero-read)
// via parseRequestFilename/scanLevel; ONLY fall back to reading (and possibly writing) content for
// legacy files missing the id in their name -> avoid reading/writing the whole folder right before delete.
- (void)purgeCacheAtRel:(NSString *)rel isFolder:(BOOL)isFolder {
    if (!_apiClient || rel.length == 0) return;
    if (!isFolder) {
        core::ParsedRequestName p = core::parseRequestFilename(rel.lastPathComponent.UTF8String);
        std::string id = p.id;
        if (id.empty()) {                         // legacy file with no id in name -> read content
            try { id = _apiClient->collection().loadRequest(rel.UTF8String).id().get(); } catch (...) {}
        }
        if (!id.empty()) _apiClient->cache().removeResponse(id);
        return;
    }
    try {
        for (const auto &c : _apiClient->collection().scanLevel(rel.UTF8String)) {
            if (c.isFolder) [self purgeCacheAtRel:N(c.relPath) isFolder:YES];
            else if (!c.id.empty()) _apiClient->cache().removeResponse(c.id);  // id from scanLevel (zero-read)
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
    for (TreeItem *t in items) { try { _apiClient->collection().remove(t.relPath.UTF8String); } catch (...) {} }
    for (NSString *p in parents) [self refreshTreeLevel:p];   // only the affected parent levels
}
- (std::string)selectedFolderRel {
    NSInteger row = _tree.selectedRow;
    if (row < 0) return "";
    TreeItem *t = [_tree itemAtRow:row];
    if (t.isFolder) return t.relPath.UTF8String;
    return [t.relPath stringByDeletingLastPathComponent].UTF8String;
}
// Default name, NO popup. Rename later via inline-rename in the tree. loadRequestAtRel
// autosaves the open request before switching.
- (void)createRequest:(core::RequestType)t name:(NSString *)name {
    if (!_apiClient) { [self toastWarn:StrToastOpenFolderFirst]; return; }
    try {
        std::string folderRel = [self selectedFolderRel];
        std::string rel = _apiClient->collection().createRequest(folderRel, t, name.UTF8String);
        [self refreshTreeLevel:N(folderRel)];   // rescan only the target folder (reveal opens it if closed)
        [self loadRequestAtRel:N(rel)];
        [self toastOk:[NSString stringWithFormat:StrFmtToastCreated, name]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)newFolder:(id)s {
    if (!_apiClient) { [self toastWarn:StrToastOpenFolderFirst]; return; }
    try {
        std::string folderRel = [self selectedFolderRel];
        _apiClient->collection().createFolder(folderRel, StrNewFolder.UTF8String);
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
        std::string dupRel = _apiClient->collection().duplicate(t.relPath.UTF8String);
        [self refreshTreeLevel:parentRel];   // the copy is at the same level -> rescan only that level
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
    try { _apiClient->collection().remove(t.relPath.UTF8String); [self refreshTreeLevel:parentRel]; }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

@end
