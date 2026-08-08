#import "windows/MainWindowControllerPrivate.h"

// Body format registry (defined lower in this file) — forward-declared so the button/template code can
// enumerate every known body mode regardless of source order.
static NSArray<NSDictionary *> *BodyModeTable(void);

@implementation MainWindowController (BodyTabs)

#pragma mark Tabs

#pragma mark Body dropdown (json/text/xml/file/form)

// Body button display name per current mode: "JSON" / "Text" / "XML" / "File" / "Form".
- (NSString *)bodyButtonTitle {
    NSString *m = _bodyMode.length ? _bodyMode : @"json";
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:m]) return d[@"label"];
    return StrBodyJson;   // none/unknown -> JSON
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
    [self applyReqPaneLanguage];                 // body modes are all JSON-side (never the Envelope tab)
    [self highlightActiveTab:_reqTabButtons active:bi];}

#pragma mark Tabs

// Lexer per pane content. Request side: language follows TAB SEMANTICS — only
// the SOAP Envelope tab (index 0) is XML; every other tab of every type edits JSON. Response side:
// content-sniff — an XML body can arrive on ANY type (SOAP, HTTP XML API), and the streaming "["-log
// stays JSON. Both setters are idempotent inside SciTextView.
- (void)applyReqPaneLanguage {
    BOOL xml = ([self requestType] == core::RequestType::Soap && _activeReqTab == 0);
    [_reqText setLanguage:(xml ? SciLanguageXml : SciLanguageJson)];
}
- (void)applyRespPaneLanguageFor:(NSString *)content {
    NSCharacterSet *ws = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    NSString *t = [content ?: @"" stringByTrimmingCharactersInSet:ws];
    [_respText setLanguage:([t hasPrefix:@"<"] ? SciLanguageXml : SciLanguageJson)];
}

- (void)reqTabClicked:(OS9BevelButton *)b { [self selectReqTab:b.tag]; }
- (void)selectReqTab:(NSInteger)tab {
    if (tab < 0 || tab >= (NSInteger)_reqBuffers.count) return;
    [self stashActiveReqBuffer];
    _activeReqTab = tab;
    if (tab < (NSInteger)_reqTabTitles.count) _leftPaneActiveTabKey = _reqTabTitles[tab];  // remember left pane
    _reqText.string = _reqBuffers[tab];
    [self applyReqPaneLanguage];
    [self highlightActiveTab:_reqTabButtons active:tab];}

- (void)respTabClicked:(OS9BevelButton *)b {
    NSInteger tab = b.tag;
    // Schema tab (GraphQL): content lives in the _gqlSchema* ivars, NOT in _respBuffers — handle BEFORE
    // the bounds guard so the tab works even when no response exists yet ( _respBuffers empty).
    if ([self requestType] == core::RequestType::GraphQl && tab >= 0 &&
        tab < (NSInteger)_respTabTitles.count && [_respTabTitles[tab] isEqualToString:StrTabSchema]) {
        _activeRespTab = tab;
        _rightPaneActiveTabKey = StrTabSchema;
        [self highlightActiveTab:_respTabButtons active:tab];
        if (!_gqlSchemaFetched && !_gqlSchemaFetching) [self fetchGqlSchema]; // shows "Fetching schema..."
        [self displayGqlSchemaPane];
        return;
    }
    if (tab < 0 || tab >= (NSInteger)_respBuffers.count) return;
    _activeRespTab = tab;
    if (tab < (NSInteger)_respTabTitles.count) _rightPaneActiveTabKey = _respTabTitles[tab];  // remember right pane
    _respText.string = _respBuffers[tab];
    [self applyRespPaneLanguageFor:_respBuffers[tab]];
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
    // Schema tab active: Pretty/Raw re-render from the cached schema (SDL vs introspection JSON) —
    // independent of _hasResp (the tab works before any send).
    if ([self respActiveTabIsSchema] && (_gqlSchemaFetched || _gqlSchemaFetching)) {
        [self displayGqlSchemaPane];
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

@end
