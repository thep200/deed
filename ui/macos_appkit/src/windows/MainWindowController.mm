#import "windows/MainWindowControllerPrivate.h"

// This file holds the core: UI building (build*), layout, per-type render, window & toast.
// The remaining groups are split into categories: +Tree / +Editor / +Send / +Config / +Stress.
// Shared ivars + imports live in MainWindowControllerPrivate.h.

@implementation MainWindowController

#pragma mark Build

- (void)showWindow {
    DeedConfig *cfg = [DeedConfig shared];
    // Display font comes from Settings (app-support) — set BEFORE building widgets.
    { core::AppConfigStore a; a.setDefaults([self appDefaultsFromEnv]); core::AppConfig c = a.load();
      [OS9Theme setConfiguredFontName:N(c.fontName) size:c.fontSize]; }
    // Button style: new (btn-new.svg) by default, or classic (button.svg) via .env.
    [OS9Theme setButtonStyleName:[cfg stringFor:@"BUTTON_STYLE" def:@"new"]];
    NSRect frame = NSMakeRect(0, 0, [cfg floatFor:@"WINDOW_WIDTH" def:1040], [cfg floatFor:@"WINDOW_HEIGHT" def:680]);
    // Window corners: SQUARE_CORNERS=1 (default) -> borderless SQUARE corners, OS9 style.
    // =0 -> system titled window (rounded corners). Title/buttons are always drawn by OS9TitleBar.
    BOOL square = [cfg boolFor:@"SQUARE_CORNERS" def:YES];
    if (square) {
        // + Miniaturizable: allow minimizing into the Dock (genie) while staying borderless -> SQUARE corners.
        _window = [[OS9Window alloc] initWithContentRect:frame
                                               styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable |
                                                          NSWindowStyleMaskMiniaturizable)
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    } else {
        _window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                                                         NSWindowStyleMaskFullSizeContentView)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        _window.titlebarAppearsTransparent = YES;
        _window.titleVisibility = NSWindowTitleHidden;
        [_window standardWindowButton:NSWindowCloseButton].hidden = YES;
        [_window standardWindowButton:NSWindowMiniaturizeButton].hidden = YES;
        [_window standardWindowButton:NSWindowZoomButton].hidden = YES;
    }
    _window.movableByWindowBackground = NO;
    _window.releasedWhenClosed = NO;   // §4: controller owns _window (ARC); don't let close auto-release it
    _window.delegate = self;
    _window.minSize = NSMakeSize([cfg floatFor:@"WINDOW_MIN_WIDTH" def:820], [cfg floatFor:@"WINDOW_MIN_HEIGHT" def:520]);
    // Bright platinum app -> force Aqua so text/fields aren't washed white by system Dark Mode.
    _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];

    OS9BackgroundView *content = [[OS9BackgroundView alloc] initWithFrame:frame];
    _window.contentView = content;

    _treeW = [cfg floatFor:@"SIDEBAR_WIDTH" def:230];
    _reqW = 0; // computed on first relayout

    [self buildChrome];
    _mainPane = [[OS9BackgroundView alloc] initWithFrame:NSZeroRect];
    [content addSubview:_mainPane];
    _configPane = [[OS9BackgroundView alloc] initWithFrame:NSZeroRect];
    _configPane.hidden = YES;
    [content addSubview:_configPane];

    [self buildTree];
    [self buildEditors];
    [self buildStatusBar];
    [self buildToolbar];
    [self buildDividers];
    [self buildConfigPane];
    [self buildToast];

    [self setRequestType:core::RequestType::Http];
    [self setHasRequest:NO];
    [self relayout];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [self updateStatus:@""];
    [self restoreLastCollection];
}

// Reopen the most recent collection folder (saved in app-support). Skipped in test mode.
- (void)restoreLastCollection {
    if (getenv("APICLIENT_OPEN")) return; // test affordance opens its own folder
    core::AppConfigStore appCfg;           // default: ~/Library/Application Support/deed/config.json
    appCfg.setDefaults([self appDefaultsFromEnv]);
    std::string last = appCfg.load().lastCollectionRoot;
    if (last.empty()) return;
    NSString *p = N(last);
    BOOL isDir = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&isDir] && isDir)
        [self openCollectionRoot:p];
}

- (void)buildChrome {
    _titleBar = [[OS9TitleBar alloc] initWithFrame:NSMakeRect(0, 0, 1040, 21)];
    _titleBar.title = @"";
    _titleBar.closeTarget = self;
    _titleBar.closeAction = @selector(closeWindow:);
    _titleBar.zoomTarget = self;
    _titleBar.zoomAction = @selector(zoomToggle:);
    _titleBar.collapseTarget = self;
    _titleBar.collapseAction = @selector(collapseToggle:);   // windowshade (borderless can't minimize)
    [_window.contentView addSubview:_titleBar];
}

- (void)buildToast { _toasts = [NSMutableArray array]; }

// Disable all macOS auto-input features on an NSTextView (including the field editor).
// Goal: prevent system autofill/spell/completion -> no helper subprocesses spawned.
- (void)disableAutoFeatures:(NSTextView *)tv {
    tv.continuousSpellCheckingEnabled = NO;
    tv.grammarCheckingEnabled = NO;
    tv.automaticSpellingCorrectionEnabled = NO;
    tv.automaticQuoteSubstitutionEnabled = NO;
    tv.automaticDashSubstitutionEnabled = NO;
    tv.automaticTextReplacementEnabled = NO;
    tv.automaticDataDetectionEnabled = NO;
    tv.automaticLinkDetectionEnabled = NO;
    if ([tv respondsToSelector:@selector(setAutomaticTextCompletionEnabled:)])
        tv.automaticTextCompletionEnabled = NO;   // macOS 10.12.2+
}

// NSTextFields use the window's SHARED field editor. Returns a field editor with all
// auto-features disabled -> applies to EVERY text field (URL, env, settings) in the window.
- (id)windowWillReturnFieldEditor:(NSWindow *)sender toObject:(id)client {
    if (!_fieldEditor) {
        _fieldEditor = [[NSTextView alloc] initWithFrame:NSZeroRect];
        _fieldEditor.fieldEditor = YES;
        [self disableAutoFeatures:_fieldEditor];
    }
    return _fieldEditor;
}

- (void)styleScroller:(NSScrollView *)sc {
    // OVERLAY: hidden, shown only on scroll events then auto-hides. OS9Scroller always draws a
    // FULL-WIDTH thumb (not hover-dependent), so it never goes "thin -> swells on hover".
    sc.scrollerStyle = NSScrollerStyleOverlay;
    sc.autohidesScrollers = YES;
    sc.scrollerKnobStyle = NSScrollerKnobStyleDefault;
    sc.hasVerticalScroller = YES;
    sc.verticalScroller = [[OS9Scroller alloc] initWithFrame:NSMakeRect(0, 0, 16, 100)];
    if (sc.hasHorizontalScroller)
        sc.horizontalScroller = [[OS9Scroller alloc] initWithFrame:NSMakeRect(0, 0, 100, 16)];
}

- (void)buildTree {
    _openButton = [[OS9BevelButton alloc] initWithTitle:StrOpenFolder target:self action:@selector(openFolder:)];
    [_mainPane addSubview:_openButton];   // centered path label (default)

    _treeInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_mainPane addSubview:_treeInset];
    _treeScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _treeScroll.hasVerticalScroller = YES;
    _treeScroll.borderType = NSNoBorder;        // serrated border drawn by OS9SerratedInset
    [self styleScroller:_treeScroll];
    _treeScroll.backgroundColor = [NSColor whiteColor];

    _tree = [[DeedOutlineView alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    col.width = 200;
    [_tree addTableColumn:col];
    _tree.outlineTableColumn = col;
    _tree.headerView = nil;
    _tree.rowHeight = 20;                  // room for 16px icon + row separator line (SPEC §2)
    _tree.indentationPerLevel = 14;        // one gutter indent per level -> child triangle under parent label
    _tree.allowsMultipleSelection = YES;   // multi-select to delete at once
    // TASK 3: DISABLE default blue highlight -> draw a light GRAY background ourselves (row view below).
    _tree.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;
    _tree.dataSource = self;
    _tree.delegate = self;
    _tree.target = self;
    _tree.action = @selector(treeClicked:);          // click folder -> fold/unfold
    _tree.doubleAction = @selector(treeDoubleClicked:); // dbl: empty area -> new HTTP; on a row -> rename
    _expandedFolders = [NSMutableSet set];
    _tree.backgroundColor = [NSColor whiteColor];
    [_tree registerForDraggedTypes:@[ kTreeDragType ]]; // drag-and-drop move
    __weak MainWindowController *weakSelf = self;
    _tree.menuProvider = ^NSMenu *(NSInteger row) { return [weakSelf contextMenuForRow:row]; };
    _treeScroll.documentView = _tree;
    [_treeInset addSubview:_treeScroll];
    _roots = [NSMutableArray array];
}

- (void)buildEditors {
    // (3) request: editable Scintilla editor. No need to stash on every keystroke:
    // stashActiveReqBuffer runs at EVERY buffer-read point (tab switch / send / mode change),
    // so the current tab's buffer is always up to date before use (avoids O(n) copy per key).
    _reqInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_mainPane addSubview:_reqInset];
    _reqText = [[SciTextView alloc] initEditable:YES];
    [_reqInset addSubview:_reqText];
    _reqBuffers = [NSMutableArray array];
    _reqTabButtons = [NSMutableArray array];

    // (4) response: read-only Scintilla editor.
    _respInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_mainPane addSubview:_respInset];
    _respText = [[SciTextView alloc] initEditable:NO];
    [_respInset addSubview:_respText];
    _respBuffers = [NSMutableArray array];
    _respTabButtons = [NSMutableArray array];
    _prettyMode = 0;

    // Left pane: cURL button (Format JSON moved to the editor's right-click menu).
    _curlButton = [[OS9BevelButton alloc] initWithTitle:StrBtnCurl target:self action:@selector(copyAsCurl:)];
    _curlButton.toolTip = StrTipCurl;
    [_mainPane addSubview:_curlButton];
}

// Label + body transform per the pretty button's current mode.
- (NSString *)prettyTitle { return @[ StrViewPretty, StrViewRaw, StrViewEncode, StrViewDecode ][_prettyMode]; }
- (NSString *)applyView:(const std::string &)body { return [self applyView:body mode:_prettyMode]; }
// Transform body per an explicit `mode` (reads NO ivars) -> safe to call from a background thread (U2).
- (NSString *)applyView:(const std::string &)body mode:(int)mode {
    switch (mode) {
        case 1: return N(core::fieldcodec::formatJson(body, false));
        case 2: return N(core::fieldcodec::jsonEncodeString(body));
        case 3: return N(core::fieldcodec::jsonDecodeString(body));
        default: return N(core::fieldcodec::formatJson(body, true));
    }
}

- (void)buildStatusBar {
    _statusBar = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_mainPane addSubview:_statusBar];
    _statusLabel = OS9CenteredLabel(@"");
    _statusLabel.alignment = NSTextAlignmentCenter;   // center horizontally + vertically
    [_mainPane addSubview:_statusLabel];
}

- (void)buildToolbar {
    _settingButton = [[OS9BevelButton alloc] initWithTitle:@"" target:self action:@selector(settingClicked:)];
    _settingButton.icon = OS9GearImage(16);   // classic gear instead of "Setting" text (centered)
    _settingButton.toolTip = StrTipSettings;
    _envButton = [[OS9BevelButton alloc] initWithTitle:StrEnvLocal target:self action:@selector(envClicked:)];
    _envButton.dropdown = YES;   // show dropdown arrow like method
    _sendButton = [[OS9BevelButton alloc] initWithTitle:@"" target:self action:@selector(sendRequest:)];
    _sendButton.isDefault = YES;
    _sendButton.icon = OS9SendImage(16);   // paper-plane icon instead of "Send" label
    _sendButton.toolTip = StrTipSend;
    _cancelButton = [[OS9BevelButton alloc] initWithTitle:StrCancel target:self action:@selector(cancelClicked:)];

    // gRPC: proto source = dropdown (Reflection | .proto). Only 2 choices.
    _protoPopup = [[OS9PopupButton alloc] initWithItems:@[ StrProtoReflection, StrProtoFile ]
                                                 target:self action:@selector(protoModeChanged:)];
    _protoPopup.toolTip = StrTipProtoSource;

    // gRPC TLS is now part of the per-request Config tab (RequestConfig.tls); no toolbar toggle.
    // (OS9Toggle widget kept in the tree for future reuse.)

    // gRPC: pick the service/RPC the server provides (placed before the Send button).
    _servicePopup = [[OS9PopupButton alloc] initWithItems:@[ StrNoRpc ]
                                                   target:self action:@selector(serviceMethodChanged:)];
    // On click -> actively check the host for RPCs, then pop the menu (reflection needs network IO).
    __weak MainWindowController *wsForRpc = self;
    _servicePopup.onClick = ^{
        MainWindowController *s = wsForRpc; if (!s) return;
        // First open fetches the full list; afterwards reuse it (no network) until invalidated
        // (request switch / URL or proto change / a send error).
        if (s->_grpcMethodsFetched && !s->_grpcMethods.empty()) [s->_servicePopup openMenu];
        else [s fetchGrpcMethodsThenOpen:YES];
    };

    _methodPopup = [[OS9PopupButton alloc] initWithItems:@[ StrMethodGet, StrMethodPost, StrMethodPut, StrMethodPatch, StrMethodDelete, StrMethodHead, StrMethodOptions ]
                                                  target:self action:@selector(methodChanged:)];

    // URL field: NO native bezel -> wrapped in OS9SerratedInset (retro serrated corners).
    _urlInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    _urlField = [[NSTextField alloc] initWithFrame:NSZeroRect];
    _urlField.font = [OS9Theme monoFont];
    _urlField.placeholderString = StrPhUrl;
    _urlField.target = self;
    _urlField.action = @selector(urlCommitted:);
    _urlField.bezeled = NO;
    _urlField.bordered = NO;
    _urlField.drawsBackground = NO;                  // white background drawn by OS9InsetView
    _urlField.textColor = [NSColor blackColor];      // black text on white
    _urlField.focusRingType = NSFocusRingTypeNone;
    _urlField.usesSingleLineMode = YES;              // no line wrapping
    _urlField.cell.wraps = NO;
    _urlField.cell.scrollable = YES;
    _urlField.lineBreakMode = NSLineBreakByTruncatingTail;
    _urlField.delegate = self;   // controlTextDidChange: -> detect cURL/grpcurl paste
    [_urlInset addSubview:_urlField];

    for (NSView *v in @[ _settingButton, _envButton, _sendButton, _cancelButton, _protoPopup, _servicePopup, _methodPopup, _urlInset ])
        [_mainPane addSubview:v];
}

- (void)buildDividers {
    __weak MainWindowController *weakSelf = self;
    _divTree = [[OS9Divider alloc] initWithFrame:NSZeroRect];
    _divTree.onDrag = ^(CGFloat dx) {
        MainWindowController *s = weakSelf; if (!s) return;
        s->_treeW += dx; [s relayout];
    };
    [_mainPane addSubview:_divTree];

    _divResp = [[OS9Divider alloc] initWithFrame:NSZeroRect];
    _divResp.onDrag = ^(CGFloat dx) {
        MainWindowController *s = weakSelf; if (!s) return;
        s->_reqW += dx; [s relayout];
    };
    [_mainPane addSubview:_divResp];
}

- (void)buildConfigPane {
    _backButton = [[OS9BevelButton alloc] initWithTitle:StrBtnBack target:self action:@selector(exitConfig:)];
    [_configPane addSubview:_backButton];   // screen title goes in the title bar (see updateTitle)

    _envVC = [[EnvWindowController alloc] initWithEngine:nil]; // engine set on open

    // Settings = JSON -> use SciTextView (Scintilla) like the request editor: JSON syntax coloring,
    // line numbers, Platinum theme, OS9 scrollbar. Scintilla manages its own buffer (no AppleSpell spawn).
    // Wrapped in OS9SerratedInset for the serrated border matching the Environments screen (OS9EnvGrid) + other panes.
    _settingInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_configPane addSubview:_settingInset];
    _settingEditor = [[SciTextView alloc] initEditable:YES];
    [_settingInset addSubview:_settingEditor];
}

#pragma mark Layout

- (void)relayout {
    NSRect cb = [_window.contentView bounds];
    CGFloat W = cb.size.width, H = cb.size.height;
    CGFloat titleH = 21;   // §2 spec: title bar fixed at 21px tall (incl. border)
    _titleBar.frame = NSMakeRect(0, 0, W, titleH);
    _mainPane.frame = NSMakeRect(0, titleH, W, H - titleH);
    _configPane.frame = NSMakeRect(0, titleH, W, H - titleH);
    if (_configMode) { [self layoutConfig]; [self positionToast]; return; }

    DeedConfig *cfg = [DeedConfig shared];
    CGFloat MW = _mainPane.bounds.size.width, MH = _mainPane.bounds.size.height;
    CGFloat pad = [cfg floatFor:@"PADDING" def:8];
    // Outer side margins = outer edge of title icons -> panes/buttons align with close/zoom/hide.
    CGFloat side = [OS9TitleBar iconSideInset];
    CGFloat tabH = [cfg floatFor:@"TAB_HEIGHT" def:22];
    CGFloat toolH = [cfg floatFor:@"TOOLBAR_HEIGHT" def:40];
    CGFloat btnH = [cfg floatFor:@"BUTTON_HEIGHT" def:22];
    CGFloat statusH = 18;
    CGFloat dw = 6;

    CGFloat top = pad;
    CGFloat statusY = top + tabH + 2;
    CGFloat panesY = statusY + statusH + 2;            // closer to status -> pane extends higher
    CGFloat panesBottom = MH - toolH - 2;              // closer to toolbar -> pane extends lower

    // clamp pane widths
    CGFloat minTree = 140, minReq = 200, minResp = 220;
    CGFloat avail = MW - 2 * side - 2 * dw;
    if (_treeW < minTree) _treeW = minTree;
    if (_treeW > avail - minReq - minResp) _treeW = avail - minReq - minResp;
    CGFloat remain = avail - _treeW; // for req + resp
    if (_reqW <= 0) _reqW = remain / 2;
    if (_reqW < minReq) _reqW = minReq;
    if (_reqW > remain - minResp) _reqW = remain - minResp;
    CGFloat respW = remain - _reqW;

    CGFloat treeX = side;
    CGFloat divTreeX = treeX + _treeW;
    CGFloat reqX = divTreeX + dw;
    CGFloat divRespX = reqX + _reqW;
    CGFloat respX = divRespX + dw;

    // (1) Open + (2) tree (CRUD via right-click, no more ⋯ button) — wrapped in serrated border
    _openButton.frame = NSMakeRect(treeX, top, _treeW, tabH);
    _treeInset.frame = NSMakeRect(treeX, statusY, _treeW, panesBottom - statusY);
    _treeScroll.frame = NSInsetRect(_treeInset.bounds, 2, 2);

    // dividers (full height of panes region)
    _divTree.frame = NSMakeRect(divTreeX, statusY, dw, panesBottom - statusY);
    _divResp.frame = NSMakeRect(divRespX, panesY, dw, panesBottom - panesY);

    // (3) LEFT pane GROUP = request tabs + cURL (same row, evenly spread) + editor.
    // WebSocket and GraphQL have no cURL equivalent -> hide the button for those types.
    BOOL showCurl = (_model.type == core::RequestType::Http || _model.type == core::RequestType::Grpc);
    _curlButton.hidden = !showCurl;
    NSMutableArray<OS9BevelButton *> *leftTabGroup = [_reqTabButtons mutableCopy];
    if (_curlButton && showCurl) [leftTabGroup addObject:_curlButton];
    [self layoutTabButtons:leftTabGroup atX:reqX y:top width:_reqW height:tabH extra:0];
    _reqInset.frame = NSMakeRect(reqX, panesY, _reqW, panesBottom - panesY);
    _reqText.frame = NSInsetRect(_reqInset.bounds, 2, 2);

    // (4) RIGHT pane GROUP = response tabs + Pretty (same row) + editor
    NSMutableArray<OS9BevelButton *> *rightTabGroup = [_respTabButtons mutableCopy];
    if (_prettyButton) [rightTabGroup addObject:_prettyButton];
    [self layoutTabButtons:rightTabGroup atX:respX y:top width:respW height:tabH extra:0];
    _respInset.frame = NSMakeRect(respX, panesY, respW, panesBottom - panesY);
    _respText.frame = NSInsetRect(_respInset.bounds, 2, 2);

    // status line (span req + resp)
    CGFloat slX = reqX, slW = (respX + respW) - reqX;
    _statusBar.frame = NSMakeRect(slX, statusY, slW, statusH);
    _statusLabel.frame = NSMakeRect(slX + 8, statusY + 1, slW - 16, statusH - 2);

    // toolbar (1 row): Setting | ENV | Method/Proto | URL (stretches) | Cancel(when sending) | Send
    CGFloat ty = MH - toolH + (toolH - btnH) / 2;
    CGFloat x = side;
    CGFloat wSetting = [cfg floatFor:@"BTN_SETTING_W" def:26];   // icon-only, compact -> gear hugs left edge
    CGFloat wEnv = [cfg floatFor:@"BTN_ENV_W" def:120];
    CGFloat wMethod = [cfg floatFor:@"BTN_METHOD_W" def:92];
    CGFloat wProto = [cfg floatFor:@"BTN_PROTO_W" def:104];   // just wider than "Reflection"
    CGFloat wService = [cfg floatFor:@"BTN_SERVICE_W" def:200];
    CGFloat wSend = [cfg floatFor:@"BTN_SEND_W" def:54];
    CGFloat wCancel = [cfg floatFor:@"BTN_CANCEL_W" def:64];

    _settingButton.frame = NSMakeRect(x, ty, wSetting, btnH); x += wSetting + 6;
    _envButton.frame = NSMakeRect(x, ty, wEnv, btnH); x += wEnv + 6;
    BOOL grpc = (_model.type == core::RequestType::Grpc);
    BOOL noPopup = (_model.type == core::RequestType::WebSocket ||
                    _model.type == core::RequestType::GraphQL);   // WS/GraphQL: no method/proto popup
    _methodPopup.frame = NSMakeRect(x, ty, wMethod, btnH);
    _protoPopup.frame = NSMakeRect(x, ty, wProto, btnH);
    _methodPopup.hidden = grpc || noPopup;   // only HTTP shows the method popup
    _protoPopup.hidden = !grpc;
    // WS/GraphQL have no leading popup; HTTP advances by method width, gRPC by proto width.
    x += (grpc ? wProto : (noPopup ? 0 : wMethod)) + 6;

    // (gRPC TLS toggle removed — TLS is set in the per-request Config tab.)

    _cancelButton.hidden = !_sending;
    _servicePopup.hidden = !grpc;
    // Right group: [servicePopup (gRPC)] [Cancel (when sending)] [Send].
    CGFloat rightGroup = wSend + 6 + (_sending ? wCancel + 6 : 0) + (grpc ? wService + 6 : 0);
    CGFloat urlW = (MW - side) - x - rightGroup;
    if (urlW < 140) urlW = 140;
    _urlInset.frame = NSMakeRect(x, ty, urlW, btnH);
    // field sits inside the inset, leaving room for the border + vertically centered for one line.
    CGFloat fh = ceil([[OS9Theme monoFont] ascender] - [[OS9Theme monoFont] descender]) + 2;
    _urlField.frame = NSMakeRect(4, floor((btnH - fh) / 2), urlW - 8, fh);
    CGFloat rx = MW - side - wSend;           // right edge of the Send button
    _sendButton.frame = NSMakeRect(rx, ty, wSend, btnH);
    if (_sending) { rx -= 6 + wCancel; _cancelButton.frame = NSMakeRect(rx, ty, wCancel, btnH); }
    if (grpc) { rx -= 6 + wService; _servicePopup.frame = NSMakeRect(rx, ty, wService, btnH); }

    [self positionToast];
}

- (void)layoutTabButtons:(NSArray<OS9BevelButton *> *)buttons atX:(CGFloat)x y:(CGFloat)y width:(CGFloat)width height:(CGFloat)h extra:(CGFloat)extra {
    if (buttons.count == 0) return;
    CGFloat bw = width / buttons.count;
    CGFloat cx = x;
    for (OS9BevelButton *btn in buttons) { btn.frame = NSMakeRect(cx, y, bw - 2, h); cx += bw; }
}

- (void)layoutConfig {
    CGFloat W = _configPane.bounds.size.width, H = _configPane.bounds.size.height;
    CGFloat pad = 12;
    _backButton.frame = NSMakeRect(pad, pad, 90, 24);                 // ← Back (top-left); title in the title bar

    CGFloat top = pad + 34;
    NSRect body = NSMakeRect(pad, top, W - 2 * pad, H - top - pad);
    if (_configKind == 0) {                                          // Environments
        if (_envVC.view) { _envVC.view.frame = body; [_envVC layout]; }
    } else {                                                         // Settings
        _settingInset.frame = body;
        _settingEditor.frame = NSInsetRect(_settingInset.bounds, 2, 2);   // leave room for serrated border
    }
}

#pragma mark Conditional render by type

- (void)setRequestType:(core::RequestType)t {
    _model.type = t;
    // Config is the LAST request tab for every type (sits right before the cURL button).
    if (t == core::RequestType::Http) {
        _reqTabTitles = @[ StrTabBody, StrTabQuery, StrTabHeaders, StrTabAuth, StrTabConfig ];  // "Query" (avoid confusion with path params); Body leftmost
        _respTabTitles = @[ StrTabResponse, StrTabHeaders, StrTabRequest, StrTabCookie ];
    } else if (t == core::RequestType::WebSocket) {
        // WS: Message = frame to send (also auto-sent on connect); Headers = handshake headers; Auth.
        // Response pane = the in/out frame log array (reuses the streaming render).
        _reqTabTitles = @[ StrTabMessage, StrTabHeaders, StrTabAuth, StrTabConfig ];
        _respTabTitles = @[ StrTabMessage, StrTabRequest ];
    } else if (t == core::RequestType::GraphQL) {
        // GraphQL: Query document + Variables (JSON) + Headers + Auth. query/mutation -> normal response pane.
        _reqTabTitles = @[ StrTabGqlQuery, StrTabVariables, StrTabHeaders, StrTabAuth, StrTabConfig ];
        _respTabTitles = @[ StrTabResponse, StrTabRequest ];
    } else {
        _reqTabTitles = @[ StrTabMessage, StrTabMetadata, StrTabAuth, StrTabConfig ];
        _respTabTitles = @[ StrTabMessage, StrTabRequest ];
    }
    [self rebuildTabButtons];
}

- (void)rebuildTabButtons {
    for (OS9BevelButton *b in _reqTabButtons) [b removeFromSuperview];
    for (OS9BevelButton *b in _respTabButtons) [b removeFromSuperview];
    [_reqTabButtons removeAllObjects];
    [_respTabButtons removeAllObjects];
    if (_prettyButton) [_prettyButton removeFromSuperview];

    NSInteger i = 0;
    for (NSString *t in _reqTabTitles) {
        OS9BevelButton *b;
        if ([t isEqualToString:StrTabBody]) {
            // Body = format-picker dropdown (json/file/form), label "Body (MODE)".
            b = [[OS9BevelButton alloc] initWithTitle:[self bodyButtonTitle]
                                               target:self action:@selector(bodyButtonClicked:)];
            b.dropdown = YES;
        } else {
            b = [[OS9BevelButton alloc] initWithTitle:t target:self action:@selector(reqTabClicked:)];
        }
        b.tag = i++; [_reqTabButtons addObject:b]; [_mainPane addSubview:b];
    }
    i = 0;
    for (NSString *t in _respTabTitles) {
        OS9BevelButton *b = [[OS9BevelButton alloc] initWithTitle:t target:self action:@selector(respTabClicked:)];
        b.tag = i++; [_respTabButtons addObject:b]; [_mainPane addSubview:b];
    }
    _prettyButton = [[OS9BevelButton alloc] initWithTitle:[self prettyTitle]
                                                   target:self action:@selector(prettyToggle:)];
    _prettyButton.toolTip = StrTipPretty;
    [_mainPane addSubview:_prettyButton];
    _activeReqTab = 0;
    _activeRespTab = 0;
}

- (void)setHasRequest:(BOOL)has {
    _hasRequest = has;
    _reqText.editable = has;
    _sendButton.enabledState = has && !_sending;
    if (!has) {
        // §2.1: resign input context before clearing editor/URL contents (avoid dangling context).
        OS9SafeEndEditing(_window, _reqText);
        OS9SafeEndEditing(_window, _respText);
        _reqText.string = @""; _respText.string = @""; _urlField.stringValue = @""; _urlPrevLen = 0;
        _currentRel.clear(); _currentId.clear();
    }
    [self updateTitle];
}

#pragma mark Window / misc

// performClose: is a no-op on borderless windows -> call windowShouldClose: (autosaves) then close directly.
- (void)closeWindow:(id)sender {
    if ([self windowShouldClose:_window]) {
        // §2.4: explicitly stop the spinner timer (the block self-cancels via weak self, but clean up now).
        [_spinTimer invalidate]; _spinTimer = nil;
        // §2/§4: release the input context of every text view/field BEFORE closing -> updateWindows
        // won't re-activate the context of a view being torn down.
        OS9SafeEndEditing(_window, nil);
        [_reqText teardown];
        [_respText teardown];
        [_settingEditor teardown];
        [_window close];
    }
}
- (BOOL)windowShouldClose:(NSWindow *)sender { [self autosaveCurrent]; return YES; } // autosave, no prompt
- (void)windowDidResize:(NSNotification *)note { [self relayout]; }
- (void)windowDidBecomeKey:(NSNotification *)note { [_titleBar setNeedsDisplay:YES]; }
- (void)windowDidResignKey:(NSNotification *)note { [_titleBar setNeedsDisplay:YES]; }

- (NSString *)abbreviatePath:(NSString *)path {
    NSString *p = path;
    NSString *home = NSHomeDirectory();
    BOOL underHome = [p hasPrefix:home];
    if (underHome) p = [p substringFromIndex:home.length];
    NSMutableArray<NSString *> *parts = [[p pathComponents] mutableCopy];
    [parts removeObject:@"/"];
    if (parts.count == 0) return underHome ? @"~" : path;
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    if (underHome) [out addObject:@"~"];
    for (NSUInteger i = 0; i < parts.count; i++) {
        NSString *c = parts[i];
        [out addObject:(i == parts.count - 1) ? c : (c.length ? [c substringToIndex:1] : c)];
    }
    return [out componentsJoinedByString:@"/"];
}

#pragma mark Toast (flat retro, stack top-right, pushed down)

- (void)toast:(NSString *)msg     { [self showToast:msg kind:0]; } // info (gray)
- (void)toastOk:(NSString *)msg   { [self showToast:msg kind:1]; } // success (green)
- (void)toastWarn:(NSString *)msg { [self showToast:msg kind:2]; } // fail (red)

- (void)showToast:(NSString *)msg kind:(NSInteger)kind {
    if (!_toasts) _toasts = [NSMutableArray array];
    NSView *cv = _window.contentView;
    OS9Toast *t = [[OS9Toast alloc] initWithMessage:msg kind:kind];
    NSSize sz = [OS9Toast sizeForMessage:msg];
    // start off-screen to the right, in the top slot -> reflow slides it in.
    t.frame = NSMakeRect(cv.bounds.size.width, 14, sz.width, sz.height);
    __weak MainWindowController *ws = self;
    __weak OS9Toast *wt = t;
    t.onClose = ^{ [ws dismissToast:wt]; };
    [cv addSubview:t positioned:NSWindowAbove relativeTo:nil];
    [_toasts addObject:t];
    while (_toasts.count > 5) {                       // cap the stack
        OS9Toast *old = _toasts.firstObject;
        [_toasts removeObjectAtIndex:0]; [old removeFromSuperview];
    }
    [self reflowToasts];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.8 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [ws dismissToast:wt]; });
}

- (void)dismissToast:(OS9Toast *)t {
    if (!t || ![_toasts containsObject:t]) return;
    [_toasts removeObject:t];
    NSRect away = t.frame; away.origin.x = _window.contentView.bounds.size.width;  // slide right + fade
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = 0.28; t.animator.frame = away; t.animator.alphaValue = 0.0;
    } completionHandler:^{ [t removeFromSuperview]; }];
    [self reflowToasts];   // remaining toasts slide down to fill the gap
}

// Stack toasts from the top-RIGHT downward: newest (end of array) on top (content flipped: small y = top).
- (void)reflowToasts {
    NSView *cv = _window.contentView;
    CGFloat W = cv.bounds.size.width;
    const CGFloat margin = 14, gap = 8;
    CGFloat top = margin;
    for (NSInteger i = (NSInteger)_toasts.count - 1; i >= 0; i--) {
        OS9Toast *t = _toasts[i];
        CGFloat tw = t.frame.size.width, th = t.frame.size.height;
        NSRect target = NSMakeRect(W - tw - margin, top, tw, th);
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.2; t.animator.frame = target; t.animator.alphaValue = 1.0;
        } completionHandler:nil];
        top = top + th + gap;
    }
}

- (void)positionToast { [self reflowToasts]; }


@end
