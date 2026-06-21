#import "windows/MainWindowController+Private.h"

@implementation MainWindowController (Editor)

#pragma mark Load / populate / sync

// Cancel + invalidate the in-flight request before switching (§8.2 step 2): a late callback
// is dropped because its handle != _currentHandle.
- (void)cancelInFlightForSwitch {
    if (!_sending) return;
    if (_engine) _engine->cancel(_currentHandle);
    _currentHandle = 0;                 // invalidate handle
    [self finishSending];
}

// §A1: loadRequest reads disk + parses JSON (large request body) -> run in BACKGROUND, apply model on main.
// The OLD request stays INTACT (editor + _model + _currentRel) until applied -> no "window" where the
// editor is cleared mid-flight (safe when arrow-keying through the tree fast). Token _loadReqSeq: only the
// LATEST result is applied -> older selections are dropped (won't show the model of a request already left).
- (void)loadRequestAtRel:(NSString *)rel {
    if (!_engine || rel.length == 0) return;
    NSString *relCopy = [rel copy];
    uint64_t token = ++_loadReqSeq;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws;
        if (!s || !s->_engine) return;
        core::RequestModel model;
        std::string err;
        try { model = s->_engine->collection().loadRequest(relCopy.UTF8String); }  // I/O OFF main
        catch (const std::exception &e) { err = e.what(); }
        __block core::RequestModel modelCopy = std::move(model);
        __block std::string errCopy = std::move(err);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws;
            if (!s2 || token != s2->_loadReqSeq) return;   // a different request was selected -> drop stale result
            if (!errCopy.empty()) { [s2 toastWarn:N(errCopy)]; return; }
            [s2 applyLoadedModel:modelCopy rel:relCopy];
        });
    });
}

// Apply the loaded model to the UI (runs on MAIN). Tear down the old request HERE (the old editor is
// still intact at this point) -> autosave reads A's correct contents, not a cleared editor from a fast switch.
- (void)applyLoadedModel:(const core::RequestModel &)model rel:(NSString *)rel {
    if (!_engine) return;
    BOOL switching = (_hasRequest && S(rel) != _currentRel);
    if (switching) {
        [self autosaveCurrent];         // 1. A dirty -> autosave (editor A still intact)
        [self cancelInFlightForSwitch]; // 2. cancel A's in-flight request
        // 2b. (CRASH_FIX §2.1) commit + deactivate A's input context BEFORE clearing Scintilla.
        // Clearing content while the editor/response is still first responder = dangling input context -> crash
        // in updateWindows (especially when typing via IME then switching requests quickly).
        OS9SafeEndEditing(_window, _reqText);
        OS9SafeEndEditing(_window, _respText);
        // 3. free A's text buffers (editor + response) + undo (§8.3)
        [_reqText clearContents];
        [_respText clearContents];
        // §8.5: release A's response — the view does NOT keep a second copy (large body -> RAM back to baseline).
        [_respBuffers removeAllObjects];
        _lastResp = core::ApiResponse{};
        _hasResp = NO;
    }
    try {
        _model = model;
        _currentRel = rel.UTF8String;
        _currentId = _model.id;          // track the open request by stable id
        _hasRequest = YES;
        _hasResp = NO;
        [self setRequestType:_model.type];
        [self populateEditorsFromModel];
        [self setHasRequest:YES];
        _engine->session().saveLastOpened(_currentRel);
        [self updateTitle];
        [self relayout];
        [self updateStatus:@""];
        _respText.string = @"";
        [self showCachedResponseForId:_currentId];   // show the most recent response (if any) — no resend
        // reveal + unfold + highlight the open node (id from filename, only expand ancestor branch).
        [self revealAndSelectRequestById:N(_currentId) relPath:N(_currentRel)];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// On open -> look up cache (RAM first, then disk) and rebuild the response pane on a hit (§0).
// §1.3: getResponse may read disk + parse JSON (large response) -> run in BACKGROUND, render on main.
// Guard _currentId == reqId at completion so we DON'T show the response of a request already left.
- (void)showCachedResponseForId:(const std::string &)reqId {
    if (reqId.empty() || !_engine) return;
    std::string idCopy = reqId;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws;
        if (!s || !s->_engine) return;
        auto rec = s->_engine->getResponse(idCopy);    // disk I/O OFF the main thread
        if (!rec) return;
        __block core::ResponseRecord recCopy = std::move(*rec);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws;
            if (!s2 || s2->_currentId != idCopy) return;   // request switched -> drop stale result
            if (recCopy.isError) {
                [s2 displayErrorKind:recCopy.errorKind message:N(recCopy.errorMessage)];
            } else {
                s2->_lastResp = recCopy.response;
                s2->_hasResp = YES;
                [s2 rebuildResponseBuffersAsync];   // format off main -> large cached response won't freeze UI (U2)
                [s2 updateStatusFromResponse:recCopy.response error:NO endMs:recCopy.receivedAt];
            }
        });
    });
}

- (void)populateEditorsFromModel {
    [_reqBuffers removeAllObjects];
    using namespace core;
    if (_model.type == RequestType::Http) {
        HttpRequest &h = _model.http;
        // Body defaults to JSON: request has no body yet (mode none) -> show as JSON.
        if (h.body.mode == "none") { h.body = Body{}; h.body.mode = "json"; }
        // Body buffer = EXACTLY the content to send (no {"mode","json"} wrapper); mode held by the app.
        [_reqBuffers addObject:[self bodyBufferFromModel:h.body]];     // 0 = Body (leftmost)
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(h.params))];   // 1
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(h.headers))];  // 2
        [_reqBuffers addObject:N(fieldcodec::authToJson(h.auth))];          // 3
        [_methodPopup selectTitle:N(h.method)];
        _urlField.stringValue = N(h.url); _urlPrevLen = _urlField.stringValue.length;
        _bodyMode = N(h.body.mode);
        [self updateBodyButtonLabel];
    } else {
        const GrpcRequest &g = _model.grpc;
        [_reqBuffers addObject:N(g.message.empty() ? "{}" : g.message)];
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(g.metadata))];
        Auth dummy;
        [_reqBuffers addObject:N(fieldcodec::authToJson(dummy))];
        _urlField.stringValue = N(g.target); _urlPrevLen = _urlField.stringValue.length;
        // reflection -> index 0; protoFiles/descriptorSet -> ".proto" (index 1).
        _protoPopup.selectedIndex = (g.protoSource.mode == "reflection") ? 0 : 1;
        [_protoPopup setNeedsDisplay:YES];
        [self showSavedGrpcMethodLabel];   // show the saved RPC (do NOT fetch; fetch on dropdown click)
    }
    // Re-apply the left pane's remembered tab (if the key exists for the current request type); else first tab.
    NSInteger li = [self tabIndexForKey:_leftPaneActiveTabKey inTitles:_reqTabTitles];
    if (li >= (NSInteger)_reqBuffers.count) li = 0;
    _activeReqTab = li;
    _reqText.string = _reqBuffers.count ? _reqBuffers[li] : @"";
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
    using namespace core;
    std::string err;
    NSArray<NSString *> *names = _reqTabTitles;
    auto fail = [&](NSInteger tab, const std::string &e) {
        if (!silent) {
            [self selectReqTab:tab];
            NSString *tn = (tab >= 0 && tab < (NSInteger)names.count) ? names[tab] : @"?";
            [self toastWarn:[NSString stringWithFormat:StrFmtToastInvalidJsonTab, tn, e.c_str()]];
        }
        return NO;
    };
    if (_model.type == RequestType::Http) {
        HttpRequest &h = _model.http;
        h.method = _methodPopup.selectedTitle.UTF8String ?: "GET";
        h.url = _urlField.stringValue.UTF8String;
        if (![self syncBodyFromBuffer:_reqBuffers[0] into:h.body err:err]) return fail(0, err);
        if (!fieldcodec::jsonToKeyValues(S(_reqBuffers[1]), h.params, err)) return fail(1, err);
        if (!fieldcodec::jsonToKeyValues(S(_reqBuffers[2]), h.headers, err)) return fail(2, err);
        if (!fieldcodec::jsonToAuth(S(_reqBuffers[3]), h.auth, err)) return fail(3, err);
    } else {
        GrpcRequest &g = _model.grpc;
        g.target = _urlField.stringValue.UTF8String;
        g.message = S(_reqBuffers[0]);
        if (!fieldcodec::jsonToKeyValues(S(_reqBuffers[1]), g.metadata, err)) return fail(1, err);
    }
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

// Autosave all changes (no prompt). Bad JSON -> skip + light warning.
- (void)autosaveCurrent {
    if (!_hasRequest || !_engine || _currentRel.empty()) return;
    if (![self resyncCurrentRelById]) return;     // request deleted/path-changed -> don't write back the old path
    if (![self syncModelFromEditors:YES]) { [self toastWarn:StrToastAutosaveFailed]; return; }
    try { _currentRel = _engine->collection().saveRequest(_currentRel, _model);  // filename may change (sync §4)
          _engine->session().saveLastOpened(_currentRel);
          // §T1: autosave touches only 1 request -> update its containing level, do NOT re-scan root + reloadData the whole tree.
          NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];
          [self refreshTreeLevel:parentRel];
          [self reselectTreeByRel:N(_currentRel)]; }   // keep highlight (filename may change due to sync §4)
    catch (...) {}
}

#pragma mark Tabs

#pragma mark Body dropdown (json/file/form)

// Body button display name per current mode: "Body (JSON)" / "Body (FILE)" / "Body (FORM)".
- (NSString *)bodyButtonTitle {
    NSString *m = _bodyMode.length ? _bodyMode : @"json";
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:m]) return d[@"label"];
    return StrBodyJson;   // text/xml/none -> JSON (keep old behavior)
}
- (NSInteger)bodyTabIndex { return [_reqTabTitles indexOfObject:StrTabBody]; } // 0 for HTTP, NSNotFound for gRPC
- (void)updateBodyButtonLabel {
    NSInteger bi = [self bodyTabIndex];
    if (bi == NSNotFound || bi >= (NSInteger)_reqTabButtons.count) return;
    _reqTabButtons[bi].title = [self bodyButtonTitle];
}
// Body template per mode — does NOT wrap a "mode" key (the app holds the mode), but DOES include
// hint keys so the user knows what to fill in.
//   json -> raw JSON.   form -> 1 sample key/value entry.   file -> object with a filePath key.
static NSString *const kFormBodyTemplate =
    @"[\n  {\n    \"key\": \"\",\n    \"value\": \"\",\n    \"enabled\": true\n  }\n]";
static NSString *const kFileBodyTemplate = @"{\n  \"filePath\": \"\"\n}";

// SINGLE SOURCE for the Body dropdown: internal mode <-> option/label/template.
// Add/remove a format by editing this table only (previously scattered across 3 if-else functions).
static NSArray<NSDictionary *> *BodyModeTable(void) {
    static NSArray *t;
    if (!t) t = @[
        @{@"mode" : @"json",            @"opt" : StrBodyJson, @"label" : StrBodyJson, @"tpl" : @"{}"},
        @{@"mode" : @"binary",          @"opt" : StrBodyFile, @"label" : StrBodyFile, @"tpl" : kFileBodyTemplate},
        @{@"mode" : @"form-urlencoded", @"opt" : StrBodyForm, @"label" : StrBodyForm, @"tpl" : kFormBodyTemplate},
    ];
    return t;
}
- (NSString *)bodyTemplateForMode:(NSString *)mode {
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:mode]) return d[@"tpl"];
    return @"{}";                                        // json (default for text/xml/none)
}

// Model.Body -> content shown in the editor (exactly what's sent, unwrapped).
- (NSString *)bodyBufferFromModel:(const core::Body &)b {
    if (b.mode == "form-urlencoded")
        return b.formUrlEncoded.empty() ? kFormBodyTemplate
                                        : N(core::fieldcodec::keyValuesToJson(b.formUrlEncoded));
    if (b.mode == "binary") {
        NSString *p = N(b.binaryFilePath);
        return [NSString stringWithFormat:@"{\n  \"filePath\": \"%@\"\n}", p ?: @""];
    }
    if (b.mode == "text") return N(b.text);
    if (b.mode == "xml") return N(b.xml);
    return N(b.json.empty() ? "{}" : b.json);            // json (default)
}

// Editor buffer -> Model.Body, parsed per _bodyMode (mode defined by the app, not in the text).
- (BOOL)syncBodyFromBuffer:(NSString *)buf into:(core::Body &)out err:(std::string &)err {
    using namespace core;
    NSString *bm = _bodyMode.length ? _bodyMode : @"json";
    Body nb;
    if ([bm isEqualToString:@"form-urlencoded"]) {
        nb.mode = "form-urlencoded";
        if (!fieldcodec::jsonToKeyValues(S(buf), nb.formUrlEncoded, err)) return NO;
    } else if ([bm isEqualToString:@"binary"]) {
        nb.mode = "binary";
        // Accept both an object {"filePath": "..."} and a bare path string (backward compat).
        NSString *t = [buf stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        id obj = t.length ? [NSJSONSerialization JSONObjectWithData:[t dataUsingEncoding:NSUTF8StringEncoding]
                                                            options:0 error:nil] : nil;
        if ([obj isKindOfClass:[NSDictionary class]]) {
            NSString *fp = ((NSDictionary *)obj)[@"filePath"] ?: ((NSDictionary *)obj)[@"path"];
            nb.binaryFilePath = fp.length ? S(fp) : "";
        } else {
            nb.binaryFilePath = S(t);   // treat the whole text as a path
        }
    } else if ([bm isEqualToString:@"text"]) {
        nb.mode = "text"; nb.text = S(buf);
    } else if ([bm isEqualToString:@"xml"]) {
        nb.mode = "xml"; nb.xml = S(buf);
    } else {
        nb.mode = "json"; nb.json = S(buf);   // raw user-entered JSON, NOT encoded into a "json" key
    }
    out = nb;
    return YES;
}

- (void)bodyButtonClicked:(OS9BevelButton *)b {
    NSInteger bi = [self bodyTabIndex];
    if (bi == NSNotFound) return;
    NSMutableArray<NSString *> *opts = [NSMutableArray array];
    NSMutableArray<NSString *> *modes = [NSMutableArray array];
    for (NSDictionary *d in BodyModeTable()) { [opts addObject:d[@"opt"]]; [modes addObject:d[@"mode"]]; }
    NSInteger sel = [modes indexOfObject:(_bodyMode.length ? _bodyMode : @"json")];
    if (sel == NSNotFound) sel = 0;
    __weak MainWindowController *ws = self;
    OS9ShowDropdown(opts, sel, b, ^(NSInteger idx) {
        MainWindowController *s = ws; if (!s) return;
        [s pickBodyMode:modes[idx]];
    });
}

- (void)pickBodyMode:(NSString *)mode {
    NSInteger bi = [self bodyTabIndex];
    if (bi == NSNotFound) return;
    [self stashActiveReqBuffer];                 // save the currently-typed content into the current tab
    if (![_bodyMode isEqualToString:mode])       // mode changed -> load the new template
        _reqBuffers[bi] = [self bodyTemplateForMode:mode];
    _bodyMode = mode;
    [self updateBodyButtonLabel];
    _activeReqTab = bi;                          // activate + show body
    if (bi < (NSInteger)_reqTabTitles.count) _leftPaneActiveTabKey = _reqTabTitles[bi];
    _reqText.string = _reqBuffers[bi];
    [self highlightActiveTab:_reqTabButtons active:bi];}

#pragma mark Tabs

- (void)reqTabClicked:(OS9BevelButton *)b { [self selectReqTab:b.tag]; }
- (void)selectReqTab:(NSInteger)tab {
    if (tab < 0 || tab >= (NSInteger)_reqBuffers.count) return;
    [self stashActiveReqBuffer];
    _activeReqTab = tab;
    if (tab < (NSInteger)_reqTabTitles.count) _leftPaneActiveTabKey = _reqTabTitles[tab];  // remember left pane
    _reqText.string = _reqBuffers[tab];
    [self highlightActiveTab:_reqTabButtons active:tab];}

- (void)respTabClicked:(OS9BevelButton *)b {
    NSInteger tab = b.tag;
    if (tab < 0 || tab >= (NSInteger)_respBuffers.count) return;
    _activeRespTab = tab;
    if (tab < (NSInteger)_respTabTitles.count) _rightPaneActiveTabKey = _respTabTitles[tab];  // remember right pane
    _respText.string = _respBuffers[tab];
    [self highlightActiveTab:_respTabButtons active:tab];
}
- (void)highlightActiveTab:(NSArray<OS9BevelButton *> *)buttons active:(NSInteger)active {
    // The active tab is drawn SUNKEN (selected) instead of with a bold border (isDefault) — fits the OS9 style.
    for (OS9BevelButton *b in buttons) b.selected = (b.tag == active);
}
- (void)prettyToggle:(id)sender {
    _prettyMode = (_prettyMode + 1) % 4;   // Pretty -> Raw -> Encode -> Decode -> ...
    _prettyButton.title = [self prettyTitle];
    [self applyPrettyToFocusedPane];
}

// Apply the current mode to the FIELD WITH THE CURSOR: request editor (open tab), setting,
// or (default) the response pane. Bevel buttons don't take focus, so firstResponder stays put.
- (void)applyPrettyToFocusedPane {
    // request editor (Scintilla) holds the cursor?
    if ([_reqText hasFocus]) {
        _reqText.string = [self applyView:S(_reqText.string)];
        [self stashActiveReqBuffer];
        return;
    }
    // setting editor (Scintilla) holds the cursor?
    if ([_settingEditor hasFocus]) {
        _settingEditor.string = [self applyView:S(_settingEditor.string)];
        return;
    }
    if (_hasResp) [self rebuildResponseBuffers]; // default: response pane
}

// Copy the current request as cURL (HTTP) / grpcurl (gRPC) to the clipboard.
- (void)copyAsCurl:(id)sender {
    if (!_hasRequest || !_engine) return;
    if (![self syncModelFromEditors:NO]) return;
    core::ResolvedRequest rr = _engine->resolveRequest(_model);
    std::string curl = core::toCurl(rr.model);
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:N(curl) forType:NSPasteboardTypeString];
    [self toastOk:StrToastCopiedCurl];
}

// Manual zoom toggle (performZoom sometimes can't un-zoom).
- (void)zoomToggle:(id)sender {
    NSScreen *sc = _window.screen ?: [NSScreen mainScreen];
    NSRect vis = sc.visibleFrame;
    if (NSEqualRects(_window.frame, vis)) {
        if (!NSIsEmptyRect(_preZoomFrame)) [_window setFrame:_preZoomFrame display:YES animate:YES];
    } else {
        _preZoomFrame = _window.frame;
        [_window setFrame:vis display:YES animate:YES];
    }
}

// Minimize: shrink the window into the Dock (genie). Borderless + Miniaturizable -> miniaturize: works.
- (void)collapseToggle:(id)sender { [_window miniaturize:nil]; }

// Recursively mark the whole view tree for redraw (self-drawing widgets read [OS9Theme uiFont]
// on every drawRect, so setNeedsDisplay is enough; do NOT cache the font).
static void OS9MarkTreeNeedsDisplay(NSView *v) {
    [v setNeedsDisplay:YES];
    for (NSView *s in v.subviews) OS9MarkTreeNeedsDisplay(s);
}

// Apply the configured font (from Settings) to every text field + redraw.
- (void)applyConfiguredFontAndRefresh {
    core::AppConfigStore a; a.setDefaults([self appDefaultsFromEnv]); core::AppConfig c = a.load();
    [OS9Theme setConfiguredFontName:N(c.fontName) size:c.fontSize];
    NSFont *mono = [OS9Theme monoFont];
    // Pass the user-configured font name straight to Scintilla (e.g. "Monaco 9").
    [_reqText setFontName:N(c.fontName) size:c.fontSize];
    [_respText setFontName:N(c.fontName) size:c.fontSize];
    [_settingEditor setFontName:N(c.fontName) size:c.fontSize];
    _urlField.font = mono;
    _tree.font = [OS9Theme uiFont];
    // NSTextField keeps its set font -> must reassign (self-drawing widgets don't need this).
    _statusLabel.font = [OS9Theme uiFont];
    [_tree reloadData];
    [self restoreExpansion:_roots];
    OS9MarkTreeNeedsDisplay(_window.contentView);   // title bar, buttons, tabs, dropdowns... redraw
    if (_envVC.view) [_envVC layout];               // Environments grid rebuilt for the new font
}

#pragma mark Editing

// Paste cURL/grpcurl into the URL field -> auto-detect -> preview -> create a new request (CURL_IMPORT.md).
// Detect "paste/drop" by a sudden length jump (>=8 chars at once) — typing grows 1 char at a time.
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object != _urlField) return;
    NSString *text = _urlField.stringValue ?: @"";
    NSUInteger len = text.length;
    NSUInteger prev = _urlPrevLen;
    _urlPrevLen = len;
    if (!_engine || len < prev + 8) return;            // not a paste -> ignore
    BOOL isCurl = _engine->looksLikeCurl(text.UTF8String);
    BOOL isGrpc = !isCurl && _engine->looksLikeGrpcurl(text.UTF8String);
    if (!isCurl && !isGrpc) return;
    // cURL: auto-import + create request immediately (toast only, NO popup). grpcurl: still confirm via popup.
    // Defer: avoid processing RIGHT inside the text-change callback (the field editor is busy).
    dispatch_async(dispatch_get_main_queue(), ^{
        if (isCurl) [self importNow:text grpc:NO];
        else        [self offerImport:text grpc:YES];
    });
}

// Import + create request immediately, no prompt; report result via toast.
// §A3: parse curl/grpcurl (possibly large) in BACKGROUND -> doesn't block main; marshal result to main.
- (void)importNow:(NSString *)text grpc:(BOOL)isGrpc {
    if (!_engine) return;
    NSString *t = [text copy];
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws; if (!s || !s->_engine) return;
        core::ImportResult r = isGrpc ? s->_engine->importFromGrpc(t.UTF8String)
                                      : s->_engine->importFromCurl(t.UTF8String);
        __block core::ImportResult rb = std::move(r);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws; if (!s2) return;
            if (!rb.ok) {
                [s2 toastWarn:[NSString stringWithFormat:StrFmtToastImportFailed,
                               isGrpc ? @"grpcurl" : @"cURL", rb.error.c_str()]];
                [s2 restoreUrlField];
                return;
            }
            [s2 applyImport:rb.model];
        });
    });
}

// Show a confirmation preview; if OK -> create a new request in the tree + open the editor.
// §A3: parse in BACKGROUND; dialog + applyImport on main.
- (void)offerImport:(NSString *)text grpc:(BOOL)isGrpc {
    if (!_engine) return;
    NSString *t = [text copy];
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws; if (!s || !s->_engine) return;
        core::ImportResult r = isGrpc ? s->_engine->importFromGrpc(t.UTF8String)
                                      : s->_engine->importFromCurl(t.UTF8String);
        __block core::ImportResult rb = std::move(r);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws; if (!s2) return;
            if (!rb.ok) {
                [s2 toastWarn:[NSString stringWithFormat:StrFmtToastImportFailed,
                               isGrpc ? @"grpcurl" : @"cURL", rb.error.c_str()]];
                return;
            }
            NSString *primary = s2->_hasRequest ? StrBtnReplaceCurrent : StrBtnCreateRequest;
            NSString *body = [NSString stringWithFormat:@"%@\n\n%@",
                              isGrpc ? StrGrpcurlDetected : StrCurlDetected,
                              [s2 importSummary:rb.model unknown:rb.unknown grpc:isGrpc]];
            NSInteger choice = [OS9Dialog confirmWithTitle:StrDlgImportTitle
                                                   message:body
                                                   buttons:@[ StrCancel, primary ]
                                             defaultButton:1 cancelButton:0
                                                      icon:OS9AlertNote
                                                    parent:s2->_window];
            if (choice == 1) [s2 applyImport:rb.model];
            else [s2 restoreUrlField];   // cancel: restore the URL field to the open request's value
        });
    });
}

- (NSString *)importSummary:(const core::RequestModel &)m unknown:(const std::vector<std::string> &)unknown grpc:(BOOL)isGrpc {
    NSMutableString *s = [NSMutableString string];
    if (isGrpc) {
        const core::GrpcRequest &g = m.grpc;
        [s appendFormat:@"target: %s\n", g.target.c_str()];
        [s appendFormat:@"%s / %s\n", g.service.c_str(), g.method.c_str()];
        [s appendFormat:@"TLS: %@ · metadata: %lu · proto: %s",
            g.tls.enabled ? @"secure" : @"plaintext",
            (unsigned long)g.metadata.size(), g.protoSource.mode.c_str()];
    } else {
        const core::HttpRequest &h = m.http;
        [s appendFormat:@"%s  %s\n", h.method.c_str(), h.url.c_str()];
        [s appendFormat:@"headers: %lu · body: %s · auth: %s",
            (unsigned long)h.headers.size(), h.body.mode.c_str(), h.auth.type.c_str()];
    }
    if (!unknown.empty()) {
        [s appendString:@"\nskipped:"];
        for (const auto &u : unknown) [s appendFormat:@" %s", u.c_str()];
    }
    return s;
}

// Suggested name: HTTP "METHOD lastPathSegment"; gRPC = method.
- (NSString *)deriveImportName:(const core::RequestModel &)m {
    if (m.type == core::RequestType::Grpc)
        return m.grpc.method.empty() ? StrImportedGrpc : N(m.grpc.method);
    NSString *url = N(m.http.url);
    NSString *path = url;
    NSRange q = [path rangeOfString:@"?"]; if (q.location != NSNotFound) path = [path substringToIndex:q.location];
    NSString *last = path.lastPathComponent;
    if (!last.length || [last containsString:@":"]) last = StrDefaultImportName; // host only
    return [NSString stringWithFormat:@"%s %@", m.http.method.c_str(), last];
}

// REPLACE the open request with the imported model (keep id/name/file, swap type + payload).
// No request open -> create new (fallback).
- (void)applyImport:(const core::RequestModel &)m {
    if (!_hasRequest || _currentRel.empty() || ![self resyncCurrentRelById]) {
        NSString *name = [self deriveImportName:m];   // fallback: no request open -> create new
        try {
            std::string folderRel = [self selectedFolderRel];   // §A4: refresh only the target level, don't reload the whole tree
            std::string rel = _engine->collection().createRequestFromModel(folderRel, m, name.UTF8String);
            [self refreshTreeLevel:N(folderRel)];
            [self loadRequestAtRel:N(rel)];
            [self toastOk:[NSString stringWithFormat:StrFmtToastImportedCreated, name]];
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
        return;
    }
    // Replace in place: keep the current request's id + name; URL/target show the parsed value.
    core::RequestModel n = m;
    n.id = _model.id;
    n.name = _model.name;
    _model = n;
    _hasResp = NO;
    [self setRequestType:_model.type];   // rebuild tabs for the new type (http <-> grpc)
    [self populateEditorsFromModel];
    [self setHasRequest:YES];
    _respText.string = @"";              // clear old response
    [self updateTitle];
    [self relayout];
    try {
        _currentRel = _engine->collection().saveRequest(_currentRel, _model);   // save + sync filename (§4)
        _engine->session().saveLastOpened(_currentRel);
        NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];  // §A4: only the request's containing level
        [self refreshTreeLevel:parentRel];
        [self reselectTreeByRel:N(_currentRel)];
        [self toastOk:[NSString stringWithFormat:StrFmtToastReplaced,
                       _model.type == core::RequestType::Grpc ? @"gRPC" : @"HTTP"]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// Restore the URL field to the open request's URL/target (avoid saving the command text by mistake).
- (void)restoreUrlField {
    if (!_hasRequest) { _urlField.stringValue = @""; _urlPrevLen = 0; return; }
    NSString *u = (_model.type == core::RequestType::Grpc) ? N(_model.grpc.target) : N(_model.http.url);
    _urlField.stringValue = u;
    _urlPrevLen = u.length;
}

- (void)methodChanged:(id)sender { }
- (void)urlCommitted:(id)sender {
    // gRPC: URL field = target -> Enter reloads the RPC list. HTTP: split out the query.
    if (_model.type == core::RequestType::Grpc) [self reloadGrpcMethods];
    else [self parseUrlQueryIntoQueryTab];
}

// Decode one query component: '+' -> space, %XX -> byte (matches Core urlutil::urlDecode).
- (NSString *)urlDecodeComponent:(NSString *)s {
    NSString *plus = [s stringByReplacingOccurrencesOfString:@"+" withString:@" "];
    return [plus stringByRemovingPercentEncoding] ?: plus;
}

// If the URL field has '?...': split the query (decode) -> append to the Query tab, URL keeps raw.
// Used when the user types a query into the URL then Enter/Send (matches cURL import behavior).
- (void)parseUrlQueryIntoQueryTab {
    NSInteger qi = [_reqTabTitles indexOfObject:StrTabQuery];
    if (qi == NSNotFound || qi >= (NSInteger)_reqBuffers.count) return;   // gRPC: no Query tab
    NSString *u = _urlField.stringValue ?: @"";
    NSRange qr = [u rangeOfString:@"?"];
    if (qr.location == NSNotFound) return;                                // no query
    NSString *raw = [u substringToIndex:qr.location];
    NSString *query = [u substringFromIndex:qr.location + 1];
    NSRange hr = [query rangeOfString:@"#"];
    if (hr.location != NSNotFound) query = [query substringToIndex:hr.location];

    [self stashActiveReqBuffer];   // make the current Query tab buffer up to date before appending

    // Take the existing entries in the Query tab (JSON array) then append the parsed ones.
    NSMutableArray *items = [NSMutableArray array];
    NSData *cur = [(_reqBuffers[qi] ?: @"[]") dataUsingEncoding:NSUTF8StringEncoding];
    id arr = cur ? [NSJSONSerialization JSONObjectWithData:cur options:0 error:nil] : nil;
    if ([arr isKindOfClass:[NSArray class]]) [items addObjectsFromArray:arr];
    for (NSString *seg in [query componentsSeparatedByString:@"&"]) {
        if (!seg.length) continue;
        NSRange eq = [seg rangeOfString:@"="];
        NSString *k = (eq.location == NSNotFound) ? seg : [seg substringToIndex:eq.location];
        NSString *v = (eq.location == NSNotFound) ? @"" : [seg substringFromIndex:eq.location + 1];
        NSString *dk = [self urlDecodeComponent:k];
        NSString *dv = [self urlDecodeComponent:v];
        // Same key already in the Query tab -> OVERWRITE with the new value (no duplicate row).
        NSUInteger hit = NSNotFound;
        for (NSUInteger idx = 0; idx < items.count; idx++) {
            id it = items[idx];
            if ([it isKindOfClass:[NSDictionary class]] && [[it objectForKey:@"key"] isEqual:dk]) { hit = idx; break; }
        }
        if (hit != NSNotFound) {
            NSMutableDictionary *md = [items[hit] mutableCopy];
            md[@"value"] = dv;
            md[@"enabled"] = @YES;
            items[hit] = md;
        } else {
            [items addObject:@{@"key" : dk, @"value" : dv, @"enabled" : @YES}];
        }
    }
    NSData *out = [NSJSONSerialization dataWithJSONObject:items options:NSJSONWritingPrettyPrinted error:nil];
    if (out) _reqBuffers[qi] = [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding];

    _urlField.stringValue = raw; _urlPrevLen = raw.length;       // URL field keeps raw
    if (_activeReqTab == qi) _reqText.string = _reqBuffers[qi];   // viewing the Query tab -> refresh
}

- (void)updateTitle {
    // Title bar is CONTEXTUAL: config screen -> "Settings"/"Environments"; otherwise -> request name.
    if (_configMode) {
        _titleBar.title = (_configKind == 0) ? StrTitleEnvironments : StrTitleSettings;
    } else {
        _titleBar.title = _hasRequest ? N(_model.name) : @"";
    }
    [_titleBar setNeedsDisplay:YES];
}

#pragma mark Save (manual ⌘S still kept)

- (void)saveRequest:(id)sender {
    if (!_hasRequest || !_engine) return;
    if (![self resyncCurrentRelById]) { [self toastWarn:StrToastRequestGone]; return; }
    if (![self syncModelFromEditors:NO]) return;
    try {
        _currentRel = _engine->collection().saveRequest(_currentRel, _model);  // filename syncs to method/name (§4)
        _engine->session().saveLastOpened(_currentRel);
        NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];  // §A4: incremental update
        [self refreshTreeLevel:parentRel];
        [self reselectTreeByRel:N(_currentRel)];
        [self toastOk:StrToastSaved];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

@end
