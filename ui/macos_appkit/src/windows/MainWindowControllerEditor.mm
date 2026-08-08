#import "windows/MainWindowControllerPrivate.h"

#include <optional>
#include <type_traits>
#include <variant>

@implementation MainWindowController (Editor)

#pragma mark Load / populate / sync

// Cancel + invalidate the in-flight request before switching: a late callback
// is dropped because its handle != _currentHandle.
- (void)cancelInFlightForSwitch {
    if (!_sending) return;
    if (_apiClient && !_apiExec.empty()) {
        if ([self requestType] == core::RequestType::WebSocket) _apiClient->closeStream(_apiExec, 1000, "bye");
        else _apiClient->cancel(_apiExec);
    }
    _currentHandle = 0;                 // invalidate handle (a late callback != _currentHandle is dropped)
    [self finishSending];
}

// loadRequest reads disk + parses JSON (large request body) -> run in BACKGROUND, apply model on main.
// The OLD request stays INTACT (editor + _model + _currentRel) until applied -> no "window" where the
// editor is cleared mid-flight (safe when arrow-keying through the tree fast). Token _loadReqSeq: only the
// LATEST result is applied -> older selections are dropped (won't show the model of a request already left).
- (void)loadRequestAtRel:(NSString *)rel {
    if (!_apiClient || rel.length == 0) return;
    NSString *relCopy = [rel copy];
    uint64_t token = ++_loadReqSeq;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws;
        if (!s || !s->_apiClient) return;
        std::optional<core::domain::RequestModel> model;
        std::string err;
        std::map<std::string, std::string> drafts;
        try {
            model = s->_apiClient->collection().loadRequest(relCopy.UTF8String);  // I/O OFF main
            drafts = s->_apiClient->collection().loadBodyDrafts(relCopy.UTF8String); // OFF main too
        } catch (const std::exception &e) { err = e.what(); }
        __block std::optional<core::domain::RequestModel> modelCopy = std::move(model);
        __block std::string errCopy = std::move(err);
        __block std::map<std::string, std::string> draftsCopy = std::move(drafts);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws;
            if (!s2 || token != s2->_loadReqSeq) return;   // a different request was selected -> drop stale result
            if (!modelCopy) { [s2 toastWarn:N(errCopy.empty() ? "load failed" : errCopy)]; return; }
            s2->_loadedBodyDrafts = std::move(draftsCopy);  // consumed by populateEditorsFromModel (no main reparse)
            [s2 applyLoadedModel:*modelCopy rel:relCopy];
        });
    });
}

// Apply the loaded model to the UI (runs on MAIN). Tear down the old request HERE (the old editor is
// still intact at this point) -> autosave reads A's correct contents, not a cleared editor from a fast switch.
- (void)applyLoadedModel:(const core::domain::RequestModel &)model rel:(NSString *)rel {
    if (!_apiClient) return;
    BOOL switching = (_hasRequest && S(rel) != _currentRel);
    if (switching) {
        [self autosaveCurrent];         // 1. A dirty -> autosave (editor A still intact)
        [self cancelInFlightForSwitch]; // 2. cancel A's in-flight request
        // 2b. commit + deactivate A's input context BEFORE clearing Scintilla.
        // Clearing content while the editor/response is still first responder = dangling input context -> crash
        // in updateWindows (especially when typing via IME then switching requests quickly).
        OS9SafeEndEditing(_window, _reqText);
        OS9SafeEndEditing(_window, _respText);
        // 3. free A's text buffers (editor + response) + undo
        [_reqText clearContents];
        [_respText clearContents];
        // Release A's response — the view does NOT keep a second copy (large body -> RAM back to baseline).
        [_respBuffers removeAllObjects];
        _lastResp = core::domain::ApiResponse{};
        _hasResp = NO;
    }
    try {
        _model = model;
        _currentRel = rel.UTF8String;
        _currentId = model.id().get();   // track the open request by stable id
        _hasRequest = YES;
        _hasResp = NO;
        [self setRequestType:model.type()];
        [self populateEditorsFromModel];
        // Baseline: the just-loaded model + drafts ARE the on-disk state -> autosave skips until edited.
        _savedModel = _model;
        _savedDrafts = [self collectBodyDrafts];
        [self setHasRequest:YES];
        _apiClient->session().saveLastOpened(_currentRel);
        [self updateTitle];
        [self relayout];
        [self updateStatus:@""];
        _respText.string = @"";
        [self showCachedResponseForId:_currentId];   // show the most recent response (if any) — no resend
        // reveal + unfold + highlight the open node (id from filename, only expand ancestor branch).
        [self revealAndSelectRequestById:N(_currentId) relPath:N(_currentRel)];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// On open -> look up cache (RAM first, then disk) and rebuild the response pane on a hit.
// getResponse may read disk + parse JSON (large response) -> run in BACKGROUND, render on main.
// Guard _currentId == reqId at completion so we DON'T show the response of a request already left.
- (void)showCachedResponseForId:(const std::string &)reqId {
    if (reqId.empty() || !_apiClient) return;
    std::string idCopy = reqId;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws;
        if (!s || !s->_apiClient) return;
        auto rec = s->_apiClient->cache().getResponse(idCopy);    // disk I/O OFF the main thread
        if (!rec) return;
        __block core::ResponseRecord recCopy = std::move(*rec);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws;
            if (!s2 || s2->_currentId != idCopy) return;   // request switched -> drop stale result
            if (recCopy.isError) {
                [s2 displayErrorKind:recCopy.errorKind message:N(recCopy.errorMessage) elapsedMs:0]; // cached: no live time
            } else {
                s2->_lastResp = recCopy.response;
                s2->_hasResp = YES;
                [s2 rebuildResponseBuffersAsync];   // format off main -> large cached response won't freeze UI
                [s2 updateStatusFromResponse:s2->_lastResp error:NO endMs:recCopy.receivedAt];
            }
        });
    });
}

- (void)populateEditorsFromModel {
    [_reqBuffers removeAllObjects];
    // Seed per-mode body drafts (UI-only "_uiBodyDrafts") so content the user typed in NON-active body modes
    // survives save/reload + switching requests. The drafts were read OFF-MAIN during the request load
    // (_loadedBodyDrafts) — do NOT re-read+parse the file here on the main thread. The ACTIVE mode below
    // overlays from the domain model (authoritative).
    _bodyDrafts = [NSMutableDictionary dictionary];
    for (auto &kv : _loadedBodyDrafts) _bodyDrafts[N(kv.first)] = N(kv.second);
    // Fresh request context -> drop any Kafka Producer/Consumer drafts from whatever was open before
    // (kafkaModeToggled: is the only other writer of these, and it never calls populateEditorsFromModel).
    _kafkaProducerReqBuffers = nil; _kafkaConsumerReqBuffers = nil;
    _kafkaProducerRespBuffers = nil; _kafkaConsumerRespBuffers = nil;
    _kafkaProducerHasResp = NO; _kafkaConsumerHasResp = NO;
    [self invalidateGqlSchema];   // schema cache is per-request; a fresh editor context drops it
    _kafkaProducerLastResp = core::domain::ApiResponse{};
    _kafkaConsumerLastResp = core::domain::ApiResponse{};
    namespace d = core::domain;
    if (!_model) { _reqText.string = @""; return; }
    const d::RequestModel &m = *_model;
    RequestTypeUi *ui = TypeUiFor(m.type());
    EditorPlan *plan = [ui populate:m];
    [_reqBuffers addObjectsFromArray:plan.buffers];
    if (plan.bodyMode) {
        // HTTP body: the domain Body holds ONE mode; other modes show their draft/template via _bodyDrafts.
        _bodyMode = plan.bodyMode;
        if (plan.bodyActiveContent.length) _bodyDrafts[_bodyMode] = plan.bodyActiveContent;
        _reqBuffers[0] = _bodyDrafts[_bodyMode] ?: [self bodyTemplateForMode:_bodyMode];
        [self updateBodyButtonLabel];
    }
    if (plan.methodTitle) [_methodPopup selectTitle:plan.methodTitle];
    if (plan.protoIndex >= 0) {
        _protoPopup.selectedIndex = plan.protoIndex;
        [_protoPopup setNeedsDisplay:YES];
    }
    if (plan.wantsSavedRpcLabel) [self showSavedGrpcMethodLabel]; // saved RPC; fetch on dropdown click
    _urlField.stringValue = plan.urlText ?: @"";
    _urlPrevLen = _urlField.stringValue.length;
    // Config tab (last for every type) = per-request timeout_ms + tls; Kafka drops "tls" (no toggle yet).
    [_reqBuffers addObject:N(ui.usesKafkaConfigSerializer
                                 ? core::serial::kafkaRequestConfigToJson(m.config())
                                 : core::serial::configToJson(m.config()))];
    // Re-apply the left pane's remembered tab (if the key exists for the current request type); else first tab.
    NSInteger li = [self tabIndexForKey:_leftPaneActiveTabKey inTitles:_reqTabTitles];
    if (li >= (NSInteger)_reqBuffers.count) li = 0;
    _activeReqTab = li;
    _reqText.string = _reqBuffers.count ? _reqBuffers[li] : @"";
    [self applyReqPaneLanguage];
    [self highlightActiveTab:_reqTabButtons active:li];}

// Tab index by KEY (title) within the titles set; no-match/nil -> 0 (first tab of that type).
- (NSInteger)tabIndexForKey:(NSString *)key inTitles:(NSArray<NSString *> *)titles {
    if (key.length) {
        NSInteger idx = [titles indexOfObject:key];
        if (idx != NSNotFound) return idx;
    }
    return 0;
}

- (void)stashActiveReqBuffer {
    // [copy] is REQUIRED: NSTextView.string returns a reference to the LIVE text storage;
    // without copy -> tab buffers all point at one mutating string -> values bleed into each other.
    if (_activeReqTab >= 0 && _activeReqTab < (NSInteger)_reqBuffers.count)
        _reqBuffers[_activeReqTab] = [(_reqText.string ?: @"") copy];
}

// Return NO on bad JSON (toast + select tab). silent=YES -> no tab change/no toast (autosave).
- (BOOL)syncModelFromEditors:(BOOL)silent {
    [self stashActiveReqBuffer];
    NSArray<NSString *> *names = _reqTabTitles;
    auto fail = [&](NSInteger tab, const std::string &e) {
        if (!silent) {
            [self selectReqTab:tab];
            NSString *tn = (tab >= 0 && tab < (NSInteger)names.count) ? names[tab] : @"?";
            [self toastWarn:[NSString stringWithFormat:StrFmtToastInvalidJsonTab, tn, e.c_str()]];
        }
        return NO;
    };
    namespace d = core::domain;
    if (!_model) return YES;                       // nothing open to sync
    const d::RequestModel &cur = *_model;
    std::string url = S(_urlField.stringValue);
    RequestTypeUi *ui = TypeUiFor(cur.type());

    TypeUiSyncFail sf;
    auto rebuilt = [ui payloadFromBuffers:_reqBuffers
                                      url:url
                              methodTitle:_methodPopup.selectedTitle
                                 bodyMode:_bodyMode
                                 oldModel:cur
                                     fail:&sf];
    if (!rebuilt) return fail(sf.tab, sf.message);
    d::RequestModel::Payload payload = std::move(*rebuilt);
    // Config tab (last buffer for every type) -> per-request timeout_ms + tls; Kafka has no "tls" key.
    NSInteger ci = (NSInteger)_reqBuffers.count - 1;
    std::string ciText = ci >= 0 ? S(_reqBuffers[ci]) : std::string("{}");
    auto cfgRes = ui.usesKafkaConfigSerializer ? core::serial::jsonToKafkaRequestConfig(ciText)
                                               : core::serial::jsonToConfig(ciText);
    if (!cfgRes.isOk()) return fail(ci, cfgRes.error().message);
    auto built = d::RequestModel::create(cur.id(), cur.name(), cur.seq(), cfgRes.take(), std::move(payload));
    if (!built.isOk()) return fail(0, built.error().message);
    _model = built.take();
    return YES;
}

// If the open request is among the deleted items -> CLOSE the editor so autosave does NOT recreate the file.
// Match by stable id (fallback relPath).
- (void)closeEditorIfDeleted:(NSArray<TreeItem *> *)deleted {
    for (TreeItem *t in deleted) {
        BOOL match = (_currentId.size() && t.requestId.length && S(t.requestId) == _currentId) ||
                     (S(t.relPath) == _currentRel && !_currentRel.empty());
        if (match) { _currentRel.clear(); _currentId.clear(); [self setHasRequest:NO]; return; }
    }
}

// Snapshot of the per-mode body drafts to persist (UI-only "_uiBodyDrafts"): every non-empty mode draft
// PLUS the active mode's live body buffer (index 0 for HTTP, authoritative for its mode). HTTP-only —
// other request types have no body modes. Lets the user keep content typed in non-active modes (the
// request still SENDS only the active mode = the domain Body).
- (std::map<std::string, std::string>)collectBodyDrafts {
    std::map<std::string, std::string> out;
    if (!_model || _model->type() != core::domain::RequestType::Http) return out;
    for (NSString *mode in _bodyDrafts) {
        NSString *content = _bodyDrafts[mode];
        if (content.length) out[S(mode)] = S(content);
    }
    NSString *active = _bodyMode.length ? _bodyMode : @"json";
    NSString *activeBody = (_reqBuffers.count > 0) ? _reqBuffers[0] : @"";
    if (activeBody.length) out[S(active)] = S(activeBody);
    return out;
}

// Autosave all changes (no prompt). Bad JSON -> skip + light warning.
- (void)autosaveCurrent {
    if (!_hasRequest || !_apiClient || _currentRel.empty()) return;
    if (![self resyncCurrentRelById]) return;     // request deleted/path-changed -> don't write back the old path
    if (![self syncModelFromEditors:YES]) { [self toastWarn:StrToastAutosaveFailed]; return; }
    if (!_model) return;
    // Perf: skip the disk write + tree refresh + reselect when nothing actually changed since
    // load/last-save. Compares the SYNCED model + body drafts (content-based, not a dirty flag) -> can't
    // miss a mutation and lose data; just avoids re-writing + rebuilding the tree on every request switch.
    std::map<std::string, std::string> drafts = [self collectBodyDrafts];
    if (_savedModel && *_savedModel == *_model && _savedDrafts == drafts) return;
    try { _currentRel = _apiClient->collection().saveRequest(_currentRel, *_model, drafts);  // filename may change (name/method sync)
          _apiClient->session().saveLastOpened(_currentRel);
          // Autosave touches only 1 request -> update its containing level, do NOT re-scan root + reloadData the whole tree.
          NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];
          [self refreshTreeLevel:parentRel];
          [self reselectTreeByRel:N(_currentRel)];     // keep highlight (filename may change due to sync)
          _savedModel = *_model; _savedDrafts = std::move(drafts); }   // snapshot the just-persisted state
    catch (...) {}
}

#pragma mark Editing

// Paste cURL/grpcurl into the URL field -> auto-detect -> preview -> create a new request.
// Detect "paste/drop" by a sudden length jump (>=8 chars at once) — typing grows 1 char at a time.
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object != _urlField) return;
    // GraphQL: any endpoint edit stales the introspected schema (next Schema-tab click re-fetches).
    if ([self requestType] == core::RequestType::GraphQl) [self invalidateGqlSchema];
    NSString *text = _urlField.stringValue ?: @"";
    NSUInteger len = text.length;
    NSUInteger prev = _urlPrevLen;
    _urlPrevLen = len;
    if (!_apiClient || len < prev + 8) return;         // not a paste (or no collection open) -> ignore
    // GraphQL first (it also recognizes a cURL whose body is a GraphQL document), then cURL, then grpcurl.
    std::optional<core::domain::ImportKind> kind = _apiClient->detectImport(text.UTF8String);
    if (!kind) return;
    // GraphQL confirms via popup; everything else auto-imports (toast, NO popup).
    // Defer: avoid processing RIGHT inside the text-change callback (the field editor is busy).
    // Stamp with the current load token; if the user switches request before this runs, drop the import.
    core::domain::ImportKind k = *kind;
    uint64_t seq = _loadReqSeq;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MainWindowController *s = ws;
        if (!s || s->_loadReqSeq != seq) return;
        if (k == core::domain::ImportKind::GraphQl) [s offerImport:text kind:k];
        else [s importNow:text kind:k];
    });
}

#pragma mark Save (manual ⌘S still kept)

- (void)saveRequest:(id)sender {
    if (!_hasRequest || !_apiClient) return;
    if (![self resyncCurrentRelById]) { [self toastWarn:StrToastRequestGone]; return; }
    if (![self syncModelFromEditors:NO] || !_model) return;
    try {
        std::map<std::string, std::string> drafts = [self collectBodyDrafts];
        _currentRel = _apiClient->collection().saveRequest(_currentRel, *_model, drafts);  // filename syncs to method/name
        _apiClient->session().saveLastOpened(_currentRel);
        NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];  // incremental update
        [self refreshTreeLevel:parentRel];
        [self reselectTreeByRel:N(_currentRel)];
        _savedModel = _model; _savedDrafts = std::move(drafts);   // in sync with disk -> autosave skips
        [self toastOk:StrToastSaved];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

@end
