#import "windows/MainWindowController+Private.h"

#if DEED_DEBUG_TOOLS
#import "debug/MainWindowController+Stress.h"

@implementation MainWindowController (Stress)

// Recursively scan the tree -> every request relPath (no folders).
static void StressCollectRels(const core::TreeNode &n, NSMutableArray<NSString *> *out) {
    if (n.isFolder) { for (const auto &c : n.children) StressCollectRels(c, out); }
    else if (!n.relPath.empty()) [out addObject:N(n.relPath)];
}

- (void)stressBootstrap {
    NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:@"deed_stress_ui"];
    [[NSFileManager defaultManager] removeItemAtPath:root error:nil];
    [[NSFileManager defaultManager] createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
    [self openCollectionRoot:root];
    if (!_engine) return;
    try {
        for (int i = 0; i < 6; i++)
            _engine->collection().createRequest("", (i % 2) ? core::RequestType::Grpc : core::RequestType::Http,
                                                 std::string("req_") + std::to_string(i));
        std::string sub = _engine->collection().createFolder("", "sub");
        for (int i = 0; i < 3; i++)
            _engine->collection().createRequest(sub, core::RequestType::Http, std::string("child_") + std::to_string(i));
    } catch (...) {}
    [self reloadTree];
}

- (NSArray<NSString *> *)stressRequestRels {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    if (_engine) { try { StressCollectRels(_engine->collection().scanTree(), out); } catch (...) {} }
    return out;
}

- (NSString *)stressOpenRequestId { return N(_currentId); }

- (uint64_t)stressRamCacheBytes {
    if (_engine && _engine->responseCache()) return _engine->responseCache()->l1UsedBytes();
    return 0;
}

- (void)stressLoadRel:(NSString *)rel {
    if (rel.length == 0) return;
    [self loadRequestAtRel:rel];
    [_window makeFirstResponder:_urlField];   // real input context on the URL field
}

- (void)stressSwitchRandom:(uint32_t)r {
    NSArray<NSString *> *rels = [self stressRequestRels];
    if (rels.count == 0) return;
    [self stressLoadRel:rels[r % rels.count]];
}

- (void)stressTypeRandom:(uint32_t)r {
    if ((r & 1) && _urlField) {            // type into URL field (NSTextField + field editor)
        [_window makeFirstResponder:_urlField];
        _urlField.stringValue = [NSString stringWithFormat:@"localhost:%u/api/%u", (r % 9000) + 1000, r];
    } else if (_reqText && _reqText.editable) {   // type into Scintilla editor
        [_window makeFirstResponder:_reqText];
        _reqText.string = [NSString stringWithFormat:@"{\n  \"k\": %u\n}", r % 1000];
    }
}

- (void)stressToggleRandomFolder:(uint32_t)r {
    NSMutableArray<TreeItem *> *folders = [NSMutableArray array];
    for (TreeItem *t in _roots) if (t.isFolder) [folders addObject:t];
    if (folders.count == 0) return;
    TreeItem *f = folders[r % folders.count];
    if ([_tree isItemExpanded:f]) [_tree collapseItem:f];
    else [_tree expandItem:f];
}

- (void)stressEnterEnv { [self enterConfig:0]; }
- (void)stressEnterSettings { [self enterConfig:1]; }
- (void)stressExitConfig { if (_configMode) [self exitConfig:nil]; }

- (void)stressPickRandomEnv:(uint32_t)r {
    if (!_engine) return;
    NSMutableArray<NSString *> *envs = [@[ @"Global" ] mutableCopy];
    try { for (const auto &n : _engine->environments().list()) if (n != "Global") [envs addObject:N(n)]; }
    catch (...) {}
    [self pickEnvNamed:envs[r % envs.count]];
}

- (void)stressInjectResponse:(BOOL)large {
    if (!_engine) return;
    NSUInteger n = large ? (20u * 1024 * 1024) : (NSUInteger)(2 * 1024);
    _lastResp = core::ApiResponse{};
    _lastResp.statusCode = 200;
    _lastResp.statusText = "OK";
    _lastResp.body = std::string(n, 'x');
    _lastResp.sizeBytes = (std::int64_t)n;
    _hasResp = YES;
    [self rebuildResponseBuffers];
    if (!_currentId.empty()) _engine->putResponse(_currentId, _lastResp);   // through cache (cap/evict)
}

- (void)stressRenameAutoDismiss:(uint32_t)r {
    if (_tree.numberOfRows <= 0) return;
    TreeItem *t = [_tree itemAtRow:(NSInteger)(r % (uint32_t)_tree.numberOfRows)];
    if (!t || t.relPath.length == 0) return;
    // Modal blocks the run loop -> schedule the abort first; the main-queue block runs in NSModalPanelRunLoopMode
    // -> dialog opens, field becomes first responder, then auto-dismisses (goes through end-editing/orderOut §2.3).
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.02 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [NSApp abortModal]; });
    [self promptRenameItem:t];
}

- (void)stressGoIdle {
    OS9SafeEndEditing(_window, nil);
    [self setHasRequest:NO];
}
@end
#endif // DEED_DEBUG_TOOLS
