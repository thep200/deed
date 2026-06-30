#import "windows/MainWindowControllerPrivate.h"

#include <optional>
#include <type_traits>
#include <variant>

// Body format registry (defined lower in this file) — forward-declared so the load/populate code can
// enumerate every known body mode regardless of source order.
static NSArray<NSDictionary *> *BodyModeTable(void);
static NSArray<NSString *> *BodyAllModes(void);

@implementation MainWindowController (Editor)

#pragma mark Load / populate / sync

// Cancel + invalidate the in-flight request before switching (§8.2 step 2): a late callback
// is dropped because its handle != _currentHandle.
- (void)cancelInFlightForSwitch {
    if (!_sending) return;
    // REFACTOR_SPEC P6: the in-flight send/stream/WS runs through IApiClient -> cancel via the exec handle.
    if (_apiClient && !_apiExec.empty()) {
        if ([self requestType] == core::RequestType::WebSocket) _apiClient->closeStream(_apiExec, 1000, "bye");
        else _apiClient->cancel(_apiExec);
    }
    _currentHandle = 0;                 // invalidate handle (a late callback != _currentHandle is dropped)
    [self finishSending];
}

// §A1: loadRequest reads disk + parses JSON (large request body) -> run in BACKGROUND, apply model on main.
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
        try { model = s->_apiClient->collection().loadRequest(relCopy.UTF8String); }  // I/O OFF main
        catch (const std::exception &e) { err = e.what(); }
        __block std::optional<core::domain::RequestModel> modelCopy = std::move(model);
        __block std::string errCopy = std::move(err);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws;
            if (!s2 || token != s2->_loadReqSeq) return;   // a different request was selected -> drop stale result
            if (!modelCopy) { [s2 toastWarn:N(errCopy.empty() ? "load failed" : errCopy)]; return; }
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

// On open -> look up cache (RAM first, then disk) and rebuild the response pane on a hit (§0).
// §1.3: getResponse may read disk + parse JSON (large response) -> run in BACKGROUND, render on main.
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
                [s2 displayErrorKind:recCopy.errorKind message:N(recCopy.errorMessage)];
            } else {
                s2->_lastResp = recCopy.response;   // cache speaks domain now (REFACTOR_SPEC D)
                s2->_hasResp = YES;
                [s2 rebuildResponseBuffersAsync];   // format off main -> large cached response won't freeze UI (U2)
                [s2 updateStatusFromResponse:s2->_lastResp error:NO endMs:recCopy.receivedAt];
            }
        });
    });
}

- (void)populateEditorsFromModel {
    [_reqBuffers removeAllObjects];
    // Seed per-mode body drafts from disk (UI-only "_uiBodyDrafts") so content the user typed in NON-active
    // body modes survives save/reload + switching requests. The ACTIVE mode below overlays from the domain
    // model (authoritative). A fresh/non-HTTP request -> empty (loadBodyDrafts returns {} on a missing key).
    _bodyDrafts = [NSMutableDictionary dictionary];
    if (!_currentRel.empty() && _apiClient) {
        for (auto &kv : _apiClient->collection().loadBodyDrafts(_currentRel))
            _bodyDrafts[N(kv.first)] = N(kv.second);
    }
    namespace d = core::domain;
    if (!_model) { _reqText.string = @""; return; }
    const d::RequestModel &m = *_model;
    if (m.type() == d::RequestType::Http) {
        const auto &h = std::get<d::HttpRequest>(m.payload());
        // Body: the domain Body holds ONE mode; decompose it to (mode, unwrapped content). Other modes have
        // no stored content (domain is single-mode) -> shown as their template on switch via _bodyDrafts.
        core::serial::EditorBody eb = core::serial::bodyToEditor(h.body());
        _bodyMode = N(eb.mode);
        if (!eb.content.empty()) _bodyDrafts[_bodyMode] = N(eb.content);
        NSString *bodyText = _bodyDrafts[_bodyMode] ?: [self bodyTemplateForMode:_bodyMode];
        [_reqBuffers addObject:bodyText];                                       // 0 = Body
        [_reqBuffers addObject:N(core::serial::paramsToJson(h.params()))];      // 1 = Params
        [_reqBuffers addObject:N(core::serial::headersToJson(h.headers()))];    // 2 = Headers
        [_reqBuffers addObject:N(core::serial::authToJson(h.auth()))];          // 3 = Auth
        [_methodPopup selectTitle:N(d::toString(h.method()))];
        _urlField.stringValue = N(h.url().raw()); _urlPrevLen = _urlField.stringValue.length;
        [self updateBodyButtonLabel];
    } else if (m.type() == d::RequestType::WebSocket) {
        const auto &w = std::get<d::WebSocketRequest>(m.payload());
        std::string frame = w.onOpenSend().empty() ? std::string() : w.onOpenSend()[0].payload;
        [_reqBuffers addObject:N(frame)];                                       // 0 = Message
        [_reqBuffers addObject:N(core::serial::headersToJson(w.headers()))];    // 1 = Headers
        [_reqBuffers addObject:N(core::serial::authToJson(w.auth()))];          // 2 = Auth
        _urlField.stringValue = N(w.url().raw()); _urlPrevLen = _urlField.stringValue.length;
    } else if (m.type() == d::RequestType::GraphQl) {
        const auto &g = std::get<d::GraphQlRequest>(m.payload());
        std::string vars = g.op().variables.text();
        [_reqBuffers addObject:N(g.op().query)];                               // 0 = Query document
        [_reqBuffers addObject:N(vars.empty() ? "{}" : vars)];                 // 1 = Variables (JSON)
        [_reqBuffers addObject:N(core::serial::headersToJson(g.headers()))];   // 2 = Headers
        [_reqBuffers addObject:N(core::serial::authToJson(g.auth()))];         // 3 = Auth
        _urlField.stringValue = N(g.url().raw()); _urlPrevLen = _urlField.stringValue.length;
    } else {
        const auto &g = std::get<d::GrpcRequest>(m.payload());
        std::string msg = g.message().text();
        [_reqBuffers addObject:N(msg.empty() ? "{}" : msg)];
        [_reqBuffers addObject:N(core::serial::metadataToJson(g.metadata()))];
        [_reqBuffers addObject:N(core::serial::authToJson(d::Auth::none()))];   // gRPC has no Auth tab (dummy)
        _urlField.stringValue = N(g.target()); _urlPrevLen = _urlField.stringValue.length;
        // reflection -> index 0; protoFiles/descriptorSet -> ".proto" (index 1).
        bool reflection = false;
        g.protoSource().match([&](auto &&p) {
            if constexpr (std::is_same_v<std::decay_t<decltype(p)>, d::ProtoReflection>) reflection = true;
        });
        _protoPopup.selectedIndex = reflection ? 0 : 1;
        [_protoPopup setNeedsDisplay:YES];
        [self showSavedGrpcMethodLabel];   // show the saved RPC (do NOT fetch; fetch on dropdown click)
    }
    // Config tab (last for every type) = per-request timeout_ms + tls.
    [_reqBuffers addObject:N(core::serial::configToJson(m.config()))];
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
    d::RequestModel::Payload payload = cur.payload();   // rebuilt below from the editor buffers

    if (cur.type() == d::RequestType::Http) {
        auto mr = d::parseHttpMethod(S(_methodPopup.selectedTitle ?: @"GET"));
        d::HttpMethod method = mr.isOk() ? mr.take() : d::HttpMethod::Get;
        d::HttpRequest::Parts p{method, d::Url::create(url).take()};   // Parts holds a Url -> brace-init
        auto br = core::serial::bodyFromEditor(S(_bodyMode.length ? _bodyMode : @"json"), S(_reqBuffers[0]));
        if (!br.isOk()) return fail(0, br.error().message);
        p.body = br.take();
        auto pr = core::serial::jsonToParams(S(_reqBuffers[1])); if (!pr.isOk()) return fail(1, pr.error().message);
        p.params = pr.take();
        auto hr = core::serial::jsonToHeaders(S(_reqBuffers[2])); if (!hr.isOk()) return fail(2, hr.error().message);
        p.headers = hr.take();
        auto ar = core::serial::jsonToAuth(S(_reqBuffers[3])); if (!ar.isOk()) return fail(3, ar.error().message);
        p.auth = ar.take();
        payload = d::HttpRequest::create(std::move(p)).take();
    } else if (cur.type() == d::RequestType::WebSocket) {
        const auto &curW = std::get<d::WebSocketRequest>(cur.payload());
        d::WebSocketRequest::Parts p{d::Url::create(url).take()};   // Parts holds a Url -> brace-init
        p.subprotocols = curW.subprotocols();
        p.defaultSendKind = curW.defaultSendKind();
        std::string frame = S(_reqBuffers[0]);
        if (!frame.empty()) p.onOpenSend.push_back({d::WsSendKind::Text, frame});
        auto hr = core::serial::jsonToHeaders(S(_reqBuffers[1])); if (!hr.isOk()) return fail(1, hr.error().message);
        p.headers = hr.take();
        auto ar = core::serial::jsonToAuth(S(_reqBuffers[2])); if (!ar.isOk()) return fail(2, ar.error().message);
        p.auth = ar.take();
        auto wr = d::WebSocketRequest::create(std::move(p)); if (!wr.isOk()) return fail(0, wr.error().message);
        payload = wr.take();
    } else if (cur.type() == d::RequestType::GraphQl) {
        const auto &curG = std::get<d::GraphQlRequest>(cur.payload());
        d::GraphQlRequest::Parts p{d::Url::create(url).take()};   // Parts holds a Url -> brace-init
        p.op = curG.op();                            // preserve operation type / operationName (not edited here)
        p.op.query = S(_reqBuffers[0]);
        p.op.variables = d::JsonText::of(S(_reqBuffers[1]));
        p.subTransport = curG.subTransport();
        p.wsProtocol = curG.wsProtocol();
        auto hr = core::serial::jsonToHeaders(S(_reqBuffers[2])); if (!hr.isOk()) return fail(2, hr.error().message);
        p.headers = hr.take();
        auto ar = core::serial::jsonToAuth(S(_reqBuffers[3])); if (!ar.isOk()) return fail(3, ar.error().message);
        p.auth = ar.take();
        auto gr = d::GraphQlRequest::create(std::move(p)); if (!gr.isOk()) return fail(0, gr.error().message);
        payload = gr.take();
    } else {
        const auto &curG = std::get<d::GrpcRequest>(cur.payload());
        d::GrpcRequest::Parts p;
        p.target = url;
        p.service = curG.service();                  // service/method/methodType set via the RPC picker
        p.method = curG.method();
        p.methodType = curG.methodType();
        p.message = d::JsonText::of(S(_reqBuffers[0]));
        auto mr = core::serial::jsonToMetadata(S(_reqBuffers[1])); if (!mr.isOk()) return fail(1, mr.error().message);
        p.metadata = mr.take();
        p.protoSource = curG.protoSource();
        p.tls = curG.tls();
        auto gr = d::GrpcRequest::create(std::move(p)); if (!gr.isOk()) return fail(0, gr.error().message);
        payload = gr.take();
    }
    // Config tab (last buffer for every type) -> per-request timeout_ms + tls.
    NSInteger ci = (NSInteger)_reqBuffers.count - 1;
    auto cfgRes = core::serial::jsonToConfig(ci >= 0 ? S(_reqBuffers[ci]) : std::string("{}"));
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
    try { _currentRel = _apiClient->collection().saveRequest(_currentRel, *_model, [self collectBodyDrafts]);  // filename may change (sync §4)
          _apiClient->session().saveLastOpened(_currentRel);
          // §T1: autosave touches only 1 request -> update its containing level, do NOT re-scan root + reloadData the whole tree.
          NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];
          [self refreshTreeLevel:parentRel];
          [self reselectTreeByRel:N(_currentRel)]; }   // keep highlight (filename may change due to sync §4)
    catch (...) {}
}

#pragma mark Tabs

#pragma mark Body dropdown (json/text/xml/file/form)

// Body button display name per current mode: "JSON" / "Text" / "XML" / "File" / "Form".
- (NSString *)bodyButtonTitle {
    NSString *m = _bodyMode.length ? _bodyMode : @"json";
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:m]) return d[@"label"];
    return StrBodyJson;   // none/unknown -> JSON (keep old behavior)
}
- (NSInteger)bodyTabIndex { return [_reqTabTitles indexOfObject:StrTabBody]; } // 0 for HTTP, NSNotFound for gRPC
- (void)updateBodyButtonLabel {
    NSInteger bi = [self bodyTabIndex];
    if (bi == NSNotFound || bi >= (NSInteger)_reqTabButtons.count) return;
    _reqTabButtons[bi].title = [self bodyButtonTitle];
}
// Body template per mode — does NOT wrap a "mode" key (the app holds the mode), but DOES include
// hint keys so the user knows what to fill in.
//   json -> raw JSON.   text -> empty.   xml -> empty <root>.   form -> 1 sample key/value entry.
//   file -> object with a filePath key.
static NSString *const kFormBodyTemplate =
    @"[\n  {\n    \"key\": \"\",\n    \"value\": \"\",\n    \"enabled\": 0\n  }\n]";
static NSString *const kFileBodyTemplate = @"{\n  \"filePath\": \"\"\n}";

// SINGLE SOURCE for the Body dropdown: internal mode <-> option/label/template.
// Add/remove a format by editing this table only (previously scattered across 3 if-else functions).
static NSArray<NSDictionary *> *BodyModeTable(void) {
    static NSArray *t;
    if (!t) t = @[
        @{@"mode" : @"json",            @"opt" : StrBodyJson, @"label" : StrBodyJson, @"tpl" : @"{}"},
        @{@"mode" : @"text",            @"opt" : StrBodyText, @"label" : StrBodyText, @"tpl" : @""},
        @{@"mode" : @"xml",             @"opt" : StrBodyXml,  @"label" : StrBodyXml,  @"tpl" : @"<root>\n</root>"},
        @{@"mode" : @"binary",          @"opt" : StrBodyFile, @"label" : StrBodyFile, @"tpl" : kFileBodyTemplate},
        @{@"mode" : @"form-urlencoded", @"opt" : StrBodyForm, @"label" : StrBodyForm, @"tpl" : kFormBodyTemplate},
    ];
    return t;
}
- (NSString *)bodyTemplateForMode:(NSString *)mode {
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:mode]) return d[@"tpl"];
    return @"{}";                                        // json (default for text/xml/none)
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
    if (![_bodyMode isEqualToString:mode]) {
        // Switching mode: keep the OLD mode's content as its draft, then restore the NEW mode's draft
        // (falling back to its template the first time). This preserves Form<->JSON<->… round-trips.
        NSString *cur = (bi < (NSInteger)_reqBuffers.count) ? _reqBuffers[bi] : @"";
        if (_bodyMode.length) _bodyDrafts[_bodyMode] = cur ?: @"";
        NSString *draft = _bodyDrafts[mode];     // seeded from the model on load / by a prior switch; nil = empty for this request
        _reqBuffers[bi] = draft ?: [self bodyTemplateForMode:mode];
    }
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
    if (!_hasRequest || !_apiClient) return;
    if (![self syncModelFromEditors:NO] || !_model) return;
    std::string curl = _apiClient->exportCurl(*_model);
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
    if (!_apiClient || len < prev + 8) return;         // not a paste (or no collection open) -> ignore
    // GraphQL first (it also recognizes a cURL whose body is a GraphQL document), then cURL, then grpcurl.
    // REFACTOR_SPEC P6: classify via IImportService (CoreApiClient) — the only import path now.
    BOOL isGraphql = NO, isCurl = NO, isGrpc = NO;
    if (auto k = _apiClient->detectImport(text.UTF8String)) {
        isGraphql = (*k == core::domain::ImportKind::GraphQl);
        isCurl    = (*k == core::domain::ImportKind::Curl);
        isGrpc    = (*k == core::domain::ImportKind::Grpcurl);
    }
    if (!isGraphql && !isCurl && !isGrpc) return;
    // cURL + grpcurl: auto-import immediately (toast, NO popup). GraphQL: confirm via popup first.
    // Defer: avoid processing RIGHT inside the text-change callback (the field editor is busy).
    // M20: stamp with the current load token; if the user switches request before this runs, drop the import.
    uint64_t seq = _loadReqSeq;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MainWindowController *s = ws;
        if (!s || s->_loadReqSeq != seq) return;
        if (isCurl) [s importNow:text kind:0];
        else if (isGrpc) [s importNow:text kind:1];
        else [s offerImport:text kind:2];
    });
}

// Run the importer for `kind` (0=cURL, 1=grpcurl, 2=GraphQL).
// REFACTOR_SPEC P6: IImportService (CoreApiClient) is the only import path — it returns a DOMAIN
// RequestModel inside core::ImportParseResult (no legacy bridge). Pure + thread-safe -> safe to call
// from the background import queue.
static core::ImportParseResult GqlRunImport(MainWindowController *self, NSInteger kind, const char *t) {
    core::ImportParseResult out;
    if (!self->_apiClient) { out.ok = false; out.error = "import unavailable"; return out; }
    core::domain::ImportKind dk = kind == 1 ? core::domain::ImportKind::Grpcurl
                                : kind == 2 ? core::domain::ImportKind::GraphQl
                                            : core::domain::ImportKind::Curl;
    auto r = self->_apiClient->importText(t, dk);
    if (!r.isOk()) { out.ok = false; out.error = r.error().message; return out; }
    out.ok = true;
    out.model = r.value().model; // domain RequestModel straight from the import service
    out.unknown = r.value().unknown;
    return out;
}
static NSString *GqlImportLabel(NSInteger kind) {
    return kind == 1 ? @"grpcurl" : kind == 2 ? @"GraphQL" : @"cURL";
}

// Import + create request immediately, no prompt; report result via toast.
// §A3: parse (possibly large) in BACKGROUND -> doesn't block main; marshal result to main.
- (void)importNow:(NSString *)text kind:(NSInteger)kind {
    if (!_apiClient) return;
    NSString *t = [text copy];
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws; if (!s || !s->_apiClient) return;
        __block core::ImportParseResult rb = GqlRunImport(s, kind, t.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws; if (!s2) return;
            if (!rb.ok || !rb.model) {
                [s2 toastWarn:[NSString stringWithFormat:StrFmtToastImportFailed,
                               GqlImportLabel(kind), rb.error.c_str()]];
                [s2 restoreUrlField];
                return;
            }
            [s2 applyImport:*rb.model];
        });
    });
}

// Show a confirmation preview; if OK -> create a new request in the tree + open the editor.
// §A3: parse in BACKGROUND; dialog + applyImport on main.
- (void)offerImport:(NSString *)text kind:(NSInteger)kind {
    if (!_apiClient) return;
    NSString *t = [text copy];
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws; if (!s || !s->_apiClient) return;
        __block core::ImportParseResult rb = GqlRunImport(s, kind, t.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws; if (!s2) return;
            if (!rb.ok || !rb.model) {
                [s2 toastWarn:[NSString stringWithFormat:StrFmtToastImportFailed,
                               GqlImportLabel(kind), rb.error.c_str()]];
                return;
            }
            NSString *primary = s2->_hasRequest ? StrBtnReplaceCurrent : StrBtnCreateRequest;
            NSString *detected = kind == 1 ? StrGrpcurlDetected : kind == 2 ? StrGraphqlDetected : StrCurlDetected;
            NSString *body = [NSString stringWithFormat:@"%@\n\n%@", detected,
                              [s2 importSummary:*rb.model unknown:rb.unknown kind:kind]];
            NSInteger choice = [OS9Dialog confirmWithTitle:StrDlgImportTitle
                                                   message:body
                                                   buttons:@[ StrCancel, primary ]
                                             defaultButton:1 cancelButton:0
                                                      icon:OS9AlertNote
                                                    parent:s2->_window];
            if (choice == 1) [s2 applyImport:*rb.model];
            else [s2 restoreUrlField];   // cancel: restore the URL field to the open request's value
        });
    });
}

- (NSString *)importSummary:(const core::domain::RequestModel &)m unknown:(const std::vector<std::string> &)unknown kind:(NSInteger)kind {
    namespace d = core::domain;
    NSMutableString *s = [NSMutableString string];
    if (kind == 2) {
        const auto &g = std::get<d::GraphQlRequest>(m.payload());
        if (!g.url().raw().empty()) [s appendFormat:@"endpoint: %s\n", g.url().raw().c_str()];
        NSString *firstLine = [N(g.op().query) componentsSeparatedByString:@"\n"].firstObject ?: @"";
        [s appendFormat:@"%@\n", firstLine];
        [s appendFormat:@"headers: %lu", (unsigned long)g.headers().size()];
    } else if (kind == 1) {
        const auto &g = std::get<d::GrpcRequest>(m.payload());
        [s appendFormat:@"target: %s\n", g.target().c_str()];
        [s appendFormat:@"%s / %s\n", g.service().empty() ? "(pick RPC)" : g.service().c_str(),
                        g.method().c_str()];
        [s appendFormat:@"TLS: %@ · metadata: %lu", m.config().tlsEnabledDefault ? @"secure" : @"plaintext",
                        (unsigned long)g.metadata().entries().size()];
    } else {
        const auto &h = std::get<d::HttpRequest>(m.payload());
        [s appendFormat:@"%s  %s\n", d::toString(h.method()).c_str(), h.url().raw().c_str()];
        [s appendFormat:@"headers: %lu", (unsigned long)h.headers().size()];
    }
    if (!unknown.empty()) {
        [s appendString:@"\nskipped:"];
        for (const auto &u : unknown) [s appendFormat:@" %s", u.c_str()];
    }
    return s;
}

// Suggested name: HTTP "METHOD lastPathSegment"; gRPC = method.
- (NSString *)deriveImportName:(const core::domain::RequestModel &)m {
    namespace d = core::domain;
    if (m.type() == d::RequestType::Grpc) {
        const auto &g = std::get<d::GrpcRequest>(m.payload());
        return g.method().empty() ? StrImportedGrpc : N(g.method());
    }
    if (m.type() == d::RequestType::GraphQl) {
        const auto &g = std::get<d::GraphQlRequest>(m.payload());
        return g.op().operationName.empty() ? N(m.name()) : N(g.op().operationName);
    }
    const auto &h = std::get<d::HttpRequest>(m.payload());
    NSString *path = N(h.url().raw());
    NSRange q = [path rangeOfString:@"?"]; if (q.location != NSNotFound) path = [path substringToIndex:q.location];
    NSString *last = path.lastPathComponent;
    if (!last.length || [last containsString:@":"]) last = StrDefaultImportName; // host only
    return [NSString stringWithFormat:@"%s %@", d::toString(h.method()).c_str(), last];
}

// REPLACE the open request with the imported model (keep id/name/file, swap type + payload).
// No request open -> create new (fallback).
- (void)applyImport:(const core::domain::RequestModel &)rawModel {
    namespace d = core::domain;
    // §9.5: proactively rewrite literal values matching the active env back to {{alias}} on import.
    d::RequestModel m = _apiClient ? _apiClient->aliasifyModel(rawModel) : rawModel;
    if (!_hasRequest || _currentRel.empty() || ![self resyncCurrentRelById]) {
        NSString *name = [self deriveImportName:m];   // fallback: no request open -> create new
        try {
            std::string folderRel = [self selectedFolderRel];   // §A4: refresh only the target level, don't reload the whole tree
            std::string rel = _apiClient->collection().createRequestFromModel(folderRel, m, name.UTF8String);
            [self refreshTreeLevel:N(folderRel)];
            [self loadRequestAtRel:N(rel)];
            [self toastOk:[NSString stringWithFormat:StrFmtToastImportedCreated, name]];
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
        return;
    }
    if (!_model) return;
    // Replace in place: keep the current request's id + name + seq; take the imported payload + config.
    d::RequestModel n =
        d::RequestModel::create(_model->id(), _model->name(), _model->seq(), m.config(), m.payload()).take();
    _model = n;
    _hasResp = NO;
    [self setRequestType:n.type()];      // rebuild tabs for the new type (http <-> grpc)
    [self populateEditorsFromModel];
    [self setHasRequest:YES];
    _respText.string = @"";              // clear old response
    [self updateTitle];
    [self relayout];
    try {
        _currentRel = _apiClient->collection().saveRequest(_currentRel, *_model, [self collectBodyDrafts]);   // save + sync filename (§4)
        _apiClient->session().saveLastOpened(_currentRel);
        NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];  // §A4: only the request's containing level
        [self refreshTreeLevel:parentRel];
        [self reselectTreeByRel:N(_currentRel)];
        [self toastOk:[NSString stringWithFormat:StrFmtToastReplaced,
                       n.type() == d::RequestType::Grpc ? @"gRPC" : @"HTTP"]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// Restore the URL field to the open request's URL/target (avoid saving the command text by mistake).
- (void)restoreUrlField {
    if (!_hasRequest || !_model) { _urlField.stringValue = @""; _urlPrevLen = 0; return; }
    std::string url;
    _model->match([&](auto &&p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, core::domain::GrpcRequest>) url = p.target();
        else url = p.url().raw(); // Http / WebSocket / GraphQl all expose url()
    });
    NSString *u = N(url);
    _urlField.stringValue = u;
    _urlPrevLen = u.length;
}

- (void)methodChanged:(id)sender { }
- (void)urlCommitted:(id)sender {
    // gRPC: URL field = target -> Enter reloads the RPC list. HTTP: split out the query.
    if ([self requestType] == core::RequestType::Grpc) [self reloadGrpcMethods];
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
        _titleBar.title = (_hasRequest && _model) ? N(_model->name()) : @"";
    }
    [_titleBar setNeedsDisplay:YES];
}

#pragma mark Save (manual ⌘S still kept)

- (void)saveRequest:(id)sender {
    if (!_hasRequest || !_apiClient) return;
    if (![self resyncCurrentRelById]) { [self toastWarn:StrToastRequestGone]; return; }
    if (![self syncModelFromEditors:NO] || !_model) return;
    try {
        _currentRel = _apiClient->collection().saveRequest(_currentRel, *_model, [self collectBodyDrafts]);  // filename syncs to method/name (§4)
        _apiClient->session().saveLastOpened(_currentRel);
        NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];  // §A4: incremental update
        [self refreshTreeLevel:parentRel];
        [self reselectTreeByRel:N(_currentRel)];
        [self toastOk:StrToastSaved];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

@end
