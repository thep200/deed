#import "windows/MainWindowController+Private.h"

// File này giữ phần lõi: dựng UI (build*), layout, render theo type, window & toast.
// Các nhóm còn lại tách sang category: +Tree / +Editor / +Send / +Config / +Stress.
// ivar + import dùng chung nằm ở MainWindowController+Private.h.

@implementation MainWindowController

#pragma mark Build

- (void)showWindow {
    DeedConfig *cfg = [DeedConfig shared];
    // Font hiển thị lấy từ Settings (app-support) — set TRƯỚC khi dựng widget.
    { core::AppConfigStore a; a.setDefaults([self appDefaultsFromEnv]); core::AppConfig c = a.load();
      [OS9Theme setConfiguredFontName:N(c.fontName) size:c.fontSize]; }
    // Kiểu nút: new (btn-new.svg) mặc định, hoặc classic (button.svg) qua .env.
    [OS9Theme setButtonStyleName:[cfg stringFor:@"BUTTON_STYLE" def:@"new"]];
    NSRect frame = NSMakeRect(0, 0, [cfg floatFor:@"WINDOW_WIDTH" def:1040], [cfg floatFor:@"WINDOW_HEIGHT" def:680]);
    // Góc cửa sổ: SQUARE_CORNERS=1 (mặc định) -> borderless góc VUÔNG kiểu OS9.
    // =0 -> titled window hệ thống (góc bo tròn). Tiêu đề/nút luôn tự vẽ ở OS9TitleBar.
    BOOL square = [cfg boolFor:@"SQUARE_CORNERS" def:YES];
    if (square) {
        // + Miniaturizable: cho phép thu vào Dock (genie) mà vẫn borderless -> góc VUÔNG.
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
    _window.releasedWhenClosed = NO;   // §4: controller sở hữu _window (ARC); không để close tự release
    _window.delegate = self;
    _window.minSize = NSMakeSize([cfg floatFor:@"WINDOW_MIN_WIDTH" def:820], [cfg floatFor:@"WINDOW_MIN_HEIGHT" def:520]);
    // App platinum sáng -> ép Aqua để text/field không bị trắng theo Dark Mode hệ thống.
    _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];

    OS9BackgroundView *content = [[OS9BackgroundView alloc] initWithFrame:frame];
    _window.contentView = content;

    _treeW = [cfg floatFor:@"SIDEBAR_WIDTH" def:230];
    _reqW = 0; // tính ở relayout lần đầu

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

// Mở lại thư mục collection gần nhất (lưu ở app-support). Bỏ qua nếu chạy chế độ test.
- (void)restoreLastCollection {
    if (getenv("APICLIENT_OPEN")) return; // affordance test sẽ tự mở folder khác
    core::AppConfigStore appCfg;           // mặc định: ~/Library/Application Support/deed/config.json
    appCfg.setDefaults([self appDefaultsFromEnv]);
    std::string last = appCfg.load().lastCollectionRoot;
    if (last.empty()) return;
    NSString *p = N(last);
    BOOL isDir = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&isDir] && isDir)
        [self openCollectionRoot:p];
}

- (void)buildChrome {
    _titleBar = [[OS9TitleBar alloc] initWithFrame:NSMakeRect(0, 0, 1040, 22)];
    _titleBar.title = @"";
    _titleBar.closeTarget = self;
    _titleBar.closeAction = @selector(closeWindow:);
    _titleBar.zoomTarget = self;
    _titleBar.zoomAction = @selector(zoomToggle:);
    _titleBar.collapseTarget = self;
    _titleBar.collapseAction = @selector(collapseToggle:);   // windowshade (borderless không minimize được)
    [_window.contentView addSubview:_titleBar];
}

- (void)buildToast { _toasts = [NSMutableArray array]; }

// Tắt toàn bộ tính năng nhập tự động của macOS trên một NSTextView (kể cả field editor).
// Mục đích: KHÔNG để hệ thống bật autofill/spell/completion -> không spawn tiến trình con hỗ trợ.
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

// NSTextField dùng FIELD EDITOR dùng chung của window. Trả về field editor đã tắt
// hết auto-features -> áp cho MỌI ô text (URL, env, settings) trong window.
- (id)windowWillReturnFieldEditor:(NSWindow *)sender toObject:(id)client {
    if (!_fieldEditor) {
        _fieldEditor = [[NSTextView alloc] initWithFrame:NSZeroRect];
        _fieldEditor.fieldEditor = YES;
        [self disableAutoFeatures:_fieldEditor];
    }
    return _fieldEditor;
}

- (void)styleScroller:(NSScrollView *)sc {
    // OVERLAY: ẩn, chỉ hiện khi có event scroll rồi tự ẩn. OS9Scroller luôn vẽ thumb
    // ĐỦ BỀ RỘNG (không phụ thuộc hover) nên không bị "mảnh -> phình khi hover".
    sc.scrollerStyle = NSScrollerStyleOverlay;
    sc.autohidesScrollers = YES;
    sc.scrollerKnobStyle = NSScrollerKnobStyleDefault;
    sc.hasVerticalScroller = YES;
    sc.verticalScroller = [[OS9Scroller alloc] initWithFrame:NSMakeRect(0, 0, 16, 100)];
    if (sc.hasHorizontalScroller)
        sc.horizontalScroller = [[OS9Scroller alloc] initWithFrame:NSMakeRect(0, 0, 100, 16)];
}

- (void)buildTree {
    _openButton = [[OS9BevelButton alloc] initWithTitle:@"Open Folder…" target:self action:@selector(openFolder:)];
    [_mainPane addSubview:_openButton];

    _treeInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_mainPane addSubview:_treeInset];
    _treeScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _treeScroll.hasVerticalScroller = YES;
    _treeScroll.borderType = NSNoBorder;        // viền răng cưa do OS9SerratedInset vẽ
    [self styleScroller:_treeScroll];
    _treeScroll.backgroundColor = [NSColor whiteColor];

    _tree = [[DeedOutlineView alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    col.width = 200;
    [_tree addTableColumn:col];
    _tree.outlineTableColumn = col;
    _tree.headerView = nil;
    _tree.rowHeight = 18;
    _tree.indentationPerLevel = 9;         // thụt ít -> sát lề trái hơn
    _tree.allowsMultipleSelection = YES;   // chọn nhiều để xoá cùng lúc
    // VIỆC 3: TẮT highlight xanh mặc định -> tự vẽ nền XÁM nhẹ (row view bên dưới).
    _tree.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;
    _tree.dataSource = self;
    _tree.delegate = self;
    _tree.target = self;
    _tree.action = @selector(treeClicked:);          // click folder -> fold/unfold
    _tree.doubleAction = @selector(treeDoubleClicked:); // dbl: vùng trống -> new HTTP; trên row -> rename
    _expandedFolders = [NSMutableSet set];
    _tree.backgroundColor = [NSColor whiteColor];
    [_tree registerForDraggedTypes:@[ kTreeDragType ]]; // kéo-thả di chuyển
    __weak MainWindowController *weakSelf = self;
    _tree.menuProvider = ^NSMenu *(NSInteger row) { return [weakSelf contextMenuForRow:row]; };
    _treeScroll.documentView = _tree;
    [_treeInset addSubview:_treeScroll];
    _roots = [NSMutableArray array];
}

- (void)buildEditors {
    // (3) request: editor Scintilla sửa được. KHÔNG cần stash mỗi phím:
    // stashActiveReqBuffer đã chạy ở MỌI điểm đọc buffer (đổi tab / gửi / đổi mode)
    // nên buffer tab hiện tại luôn được cập nhật trước khi dùng (tránh copy O(n)/phím).
    _reqInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_mainPane addSubview:_reqInset];
    _reqText = [[SciTextView alloc] initEditable:YES];
    [_reqInset addSubview:_reqText];
    _reqBuffers = [NSMutableArray array];
    _reqTabButtons = [NSMutableArray array];

    // (4) response: editor Scintilla read-only.
    _respInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_mainPane addSubview:_respInset];
    _respText = [[SciTextView alloc] initEditable:NO];
    [_respInset addSubview:_respText];
    _respBuffers = [NSMutableArray array];
    _respTabButtons = [NSMutableArray array];
    _prettyMode = 0;

    // Pane trái: nút cURL (Format JSON chuyển sang menu chuột phải trong editor).
    _curlButton = [[OS9BevelButton alloc] initWithTitle:@"cURL" target:self action:@selector(copyAsCurl:)];
    _curlButton.toolTip = @"Copy current request as cURL";
    [_mainPane addSubview:_curlButton];
}

// Nhãn + biến đổi body theo chế độ hiện tại của nút pretty.
- (NSString *)prettyTitle { return @[ @"Pretty", @"Raw", @"Encode", @"Decode" ][_prettyMode]; }
- (NSString *)applyView:(const std::string &)body {
    switch (_prettyMode) {
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
    _statusLabel.alignment = NSTextAlignmentCenter;   // căn giữa ngang + dọc
    [_mainPane addSubview:_statusLabel];
}

- (void)buildToolbar {
    _settingButton = [[OS9BevelButton alloc] initWithTitle:@"" target:self action:@selector(settingClicked:)];
    _settingButton.icon = OS9GearImage(16);   // bánh răng cổ điển thay cho chữ "Setting"
    _settingButton.toolTip = @"Settings";
    _envButton = [[OS9BevelButton alloc] initWithTitle:@"Global" target:self action:@selector(envClicked:)];
    _envButton.dropdown = YES;   // hiển thị mũi tên dropdown như method
    _sendButton = [[OS9BevelButton alloc] initWithTitle:@"" target:self action:@selector(sendRequest:)];
    _sendButton.isDefault = YES;
    _sendButton.icon = OS9SendImage(16);   // icon máy bay giấy thay cho label "Send"
    _sendButton.toolTip = @"Send  ⌘↩";
    _cancelButton = [[OS9BevelButton alloc] initWithTitle:@"Cancel" target:self action:@selector(cancelClicked:)];

    // gRPC: nguồn proto = dropdown (Reflection | .proto). Chỉ 2 lựa chọn.
    _protoPopup = [[OS9PopupButton alloc] initWithItems:@[ @"Reflection", @".proto" ]
                                                 target:self action:@selector(protoModeChanged:)];
    _protoPopup.toolTip = @"Proto source: Reflection (ask server) or load a .proto file";

    // gRPC: chọn service/RPC mà server cung cấp (đặt trước nút Send).
    _servicePopup = [[OS9PopupButton alloc] initWithItems:@[ @"No RPC" ]
                                                   target:self action:@selector(serviceMethodChanged:)];
    // Bấm vào -> chủ động check host lấy RPC rồi mới bung menu (reflection cần IO mạng).
    __weak MainWindowController *wsForRpc = self;
    _servicePopup.onClick = ^{
        MainWindowController *s = wsForRpc; if (!s) return;
        [s fetchGrpcMethodsThenOpen:YES];
    };

    _methodPopup = [[OS9PopupButton alloc] initWithItems:@[ @"GET", @"POST", @"PUT", @"PATCH", @"DELETE", @"HEAD", @"OPTIONS" ]
                                                  target:self action:@selector(methodChanged:)];

    // Ô URL: KHÔNG bezel native -> bọc trong OS9SerratedInset (góc răng cưa retro).
    _urlInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    _urlField = [[NSTextField alloc] initWithFrame:NSZeroRect];
    _urlField.font = [OS9Theme monoFont];
    _urlField.placeholderString = @"localhost:8000/api/deed";
    _urlField.target = self;
    _urlField.action = @selector(urlCommitted:);
    _urlField.bezeled = NO;
    _urlField.bordered = NO;
    _urlField.drawsBackground = NO;                  // nền trắng do OS9InsetView vẽ
    _urlField.textColor = [NSColor blackColor];      // chữ đen trên nền trắng
    _urlField.focusRingType = NSFocusRingTypeNone;
    _urlField.usesSingleLineMode = YES;              // không wrap xuống dòng
    _urlField.cell.wraps = NO;
    _urlField.cell.scrollable = YES;
    _urlField.lineBreakMode = NSLineBreakByTruncatingTail;
    _urlField.delegate = self;   // controlTextDidChange: -> phát hiện dán cURL/grpcurl
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
    _backButton = [[OS9BevelButton alloc] initWithTitle:@"←  Back" target:self action:@selector(exitConfig:)];
    [_configPane addSubview:_backButton];

    _configTitle = OS9Label(@"");
    _configTitle.font = [NSFont boldSystemFontOfSize:14];
    [_configPane addSubview:_configTitle];

    _envVC = [[EnvWindowController alloc] initWithEngine:nil]; // engine set khi mở

    // Settings = JSON -> dùng SciTextView (Scintilla) như editor request: tô màu JSON,
    // số dòng, theme Platinum, scrollbar OS9. Scintilla tự quản buffer (không spawn AppleSpell).
    _settingEditor = [[SciTextView alloc] initEditable:YES];
    [_configPane addSubview:_settingEditor];
}

#pragma mark Layout

- (void)relayout {
    NSRect cb = [_window.contentView bounds];
    CGFloat W = cb.size.width, H = cb.size.height;
    CGFloat titleH = 22;
    _titleBar.frame = NSMakeRect(0, 0, W, titleH);
    _mainPane.frame = NSMakeRect(0, titleH, W, H - titleH);
    _configPane.frame = NSMakeRect(0, titleH, W, H - titleH);
    if (_configMode) { [self layoutConfig]; [self positionToast]; return; }

    DeedConfig *cfg = [DeedConfig shared];
    CGFloat MW = _mainPane.bounds.size.width, MH = _mainPane.bounds.size.height;
    CGFloat pad = [cfg floatFor:@"PADDING" def:8];
    CGFloat tabH = [cfg floatFor:@"TAB_HEIGHT" def:22];
    CGFloat toolH = [cfg floatFor:@"TOOLBAR_HEIGHT" def:40];
    CGFloat btnH = [cfg floatFor:@"BUTTON_HEIGHT" def:22];
    CGFloat statusH = 18;
    CGFloat dw = 6;

    CGFloat top = pad;
    CGFloat statusY = top + tabH + 2;
    CGFloat panesY = statusY + statusH + 2;            // sát status hơn -> pane cao lên trên
    CGFloat panesBottom = MH - toolH - 2;              // sát toolbar hơn -> pane dài xuống dưới

    // clamp bề rộng panes
    CGFloat minTree = 140, minReq = 200, minResp = 220;
    CGFloat avail = MW - 2 * pad - 2 * dw;
    if (_treeW < minTree) _treeW = minTree;
    if (_treeW > avail - minReq - minResp) _treeW = avail - minReq - minResp;
    CGFloat remain = avail - _treeW; // cho req + resp
    if (_reqW <= 0) _reqW = remain / 2;
    if (_reqW < minReq) _reqW = minReq;
    if (_reqW > remain - minResp) _reqW = remain - minResp;
    CGFloat respW = remain - _reqW;

    CGFloat treeX = pad;
    CGFloat divTreeX = treeX + _treeW;
    CGFloat reqX = divTreeX + dw;
    CGFloat divRespX = reqX + _reqW;
    CGFloat respX = divRespX + dw;

    // (1) Open + (2) tree (CRUD qua chuột phải, không còn nút ⋯) — bọc viền răng cưa
    _openButton.frame = NSMakeRect(treeX, top, _treeW, tabH);
    _treeInset.frame = NSMakeRect(treeX, statusY, _treeW, panesBottom - statusY);
    _treeScroll.frame = NSInsetRect(_treeInset.bounds, 2, 2);

    // dividers (cao suốt vùng panes)
    _divTree.frame = NSMakeRect(divTreeX, statusY, dw, panesBottom - statusY);
    _divResp.frame = NSMakeRect(divRespX, panesY, dw, panesBottom - panesY);

    // (3) NHÓM pane trái = tab request + cURL (cùng 1 hàng, dàn đều) + editor
    NSMutableArray<OS9BevelButton *> *leftTabGroup = [_reqTabButtons mutableCopy];
    if (_curlButton) [leftTabGroup addObject:_curlButton];
    [self layoutTabButtons:leftTabGroup atX:reqX y:top width:_reqW height:tabH extra:0];
    _reqInset.frame = NSMakeRect(reqX, panesY, _reqW, panesBottom - panesY);
    _reqText.frame = NSInsetRect(_reqInset.bounds, 2, 2);

    // (4) NHÓM pane phải = tab response + Pretty (cùng 1 hàng) + editor
    NSMutableArray<OS9BevelButton *> *rightTabGroup = [_respTabButtons mutableCopy];
    if (_prettyButton) [rightTabGroup addObject:_prettyButton];
    [self layoutTabButtons:rightTabGroup atX:respX y:top width:respW height:tabH extra:0];
    _respInset.frame = NSMakeRect(respX, panesY, respW, panesBottom - panesY);
    _respText.frame = NSInsetRect(_respInset.bounds, 2, 2);

    // status line (span req + resp)
    CGFloat slX = reqX, slW = (respX + respW) - reqX;
    _statusBar.frame = NSMakeRect(slX, statusY, slW, statusH);
    _statusLabel.frame = NSMakeRect(slX + 8, statusY + 1, slW - 16, statusH - 2);

    // toolbar (1 dòng): Setting | ENV | Method/Proto | URL (giãn) | Cancel(khi gửi) | Send
    CGFloat ty = MH - toolH + (toolH - btnH) / 2;
    CGFloat x = pad;
    CGFloat wSetting = [cfg floatFor:@"BTN_SETTING_W" def:64];
    CGFloat wEnv = [cfg floatFor:@"BTN_ENV_W" def:120];
    CGFloat wMethod = [cfg floatFor:@"BTN_METHOD_W" def:92];
    CGFloat wProto = [cfg floatFor:@"BTN_PROTO_W" def:120];
    CGFloat wService = [cfg floatFor:@"BTN_SERVICE_W" def:200];
    CGFloat wSend = [cfg floatFor:@"BTN_SEND_W" def:54];
    CGFloat wCancel = [cfg floatFor:@"BTN_CANCEL_W" def:64];

    _settingButton.frame = NSMakeRect(x, ty, wSetting, btnH); x += wSetting + 6;
    _envButton.frame = NSMakeRect(x, ty, wEnv, btnH); x += wEnv + 6;
    BOOL grpc = (_model.type == core::RequestType::Grpc);
    _methodPopup.frame = NSMakeRect(x, ty, wMethod, btnH);
    _protoPopup.frame = NSMakeRect(x, ty, wProto, btnH);
    _methodPopup.hidden = grpc;
    _protoPopup.hidden = !grpc;
    x += (grpc ? wProto : wMethod) + 6;

    _cancelButton.hidden = !_sending;
    _servicePopup.hidden = !grpc;
    // Nhóm phải: [servicePopup (gRPC)] [Cancel (khi gửi)] [Send].
    CGFloat rightGroup = wSend + 6 + (_sending ? wCancel + 6 : 0) + (grpc ? wService + 6 : 0);
    CGFloat urlW = (MW - pad) - x - rightGroup;
    if (urlW < 140) urlW = 140;
    _urlInset.frame = NSMakeRect(x, ty, urlW, btnH);
    // field nằm trong inset, chừa viền + canh giữa theo chiều dọc cho 1 dòng.
    CGFloat fh = ceil([[OS9Theme monoFont] ascender] - [[OS9Theme monoFont] descender]) + 2;
    _urlField.frame = NSMakeRect(4, floor((btnH - fh) / 2), urlW - 8, fh);
    CGFloat rx = MW - pad - wSend;            // mép phải nút Send
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
    _backButton.frame = NSMakeRect(pad, pad, 90, 24);                 // ← Back (trên-trái)
    _configTitle.frame = NSMakeRect(pad + 100, pad + 2, W - pad - 110, 22);

    CGFloat top = pad + 34;
    NSRect body = NSMakeRect(pad, top, W - 2 * pad, H - top - pad);
    if (_configKind == 0) {                                          // Environments
        if (_envVC.view) { _envVC.view.frame = body; [_envVC layout]; }
    } else {                                                         // Settings
        _settingEditor.frame = body;
    }
}

#pragma mark Conditional render theo type

- (void)setRequestType:(core::RequestType)t {
    _model.type = t;
    if (t == core::RequestType::Http) {
        _reqTabTitles = @[ @"Body", @"Query", @"Headers", @"Auth" ];  // "Query" (tránh nhầm với path params); Body ngoài cùng trái
        _respTabTitles = @[ @"Response", @"Headers", @"Request", @"Cookie" ];
    } else {
        _reqTabTitles = @[ @"Message", @"Metadata", @"Auth" ];
        _respTabTitles = @[ @"Message", @"Request" ];
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
        if ([t isEqualToString:@"Body"]) {
            // Body = dropdown chọn định dạng (json/file/form), label "Body (MODE)".
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
    _prettyButton.toolTip = @"Pretty/Raw/Encode/Decode — applies to the focused pane";
    [_mainPane addSubview:_prettyButton];
    _activeReqTab = 0;
    _activeRespTab = 0;
}

- (void)setHasRequest:(BOOL)has {
    _hasRequest = has;
    _reqText.editable = has;
    _sendButton.enabledState = has && !_sending;
    if (!has) {
        // §2.1: resign input context trước khi xoá nội dung editor/URL (tránh dangling context).
        OS9SafeEndEditing(_window, _reqText);
        OS9SafeEndEditing(_window, _respText);
        _reqText.string = @""; _respText.string = @""; _urlField.stringValue = @""; _urlPrevLen = 0;
        _currentRel.clear(); _currentId.clear();
    }
    [self updateTitle];
}

#pragma mark Window / misc

// performClose: vô hiệu với window borderless -> gọi windowShouldClose: (tự lưu) rồi close trực tiếp.
- (void)closeWindow:(id)sender {
    if ([self windowShouldClose:_window]) {
        // §2.4: dừng spinner timer tường minh (block tự huỷ qua weak self, nhưng dọn ngay cho sạch).
        [_spinTimer invalidate]; _spinTimer = nil;
        // §2/§4: nhả input context của mọi text view/field TRƯỚC khi đóng -> updateWindows
        // không kích hoạt lại context của view đang bị tháo.
        OS9SafeEndEditing(_window, nil);
        [_reqText teardown];
        [_respText teardown];
        [_settingEditor teardown];
        [_window close];
    }
}
- (BOOL)windowShouldClose:(NSWindow *)sender { [self autosaveCurrent]; return YES; } // tự lưu, không hỏi
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

#pragma mark Toast (retro phẳng, stack góc phải-trên, đẩy xuống)

- (void)toast:(NSString *)msg     { [self showToast:msg kind:0]; } // info (xám)
- (void)toastOk:(NSString *)msg   { [self showToast:msg kind:1]; } // success (xanh)
- (void)toastWarn:(NSString *)msg { [self showToast:msg kind:2]; } // fail (đỏ)

- (void)showToast:(NSString *)msg kind:(NSInteger)kind {
    if (!_toasts) _toasts = [NSMutableArray array];
    NSView *cv = _window.contentView;
    OS9Toast *t = [[OS9Toast alloc] initWithMessage:msg kind:kind];
    NSSize sz = [OS9Toast sizeForMessage:msg];
    // bắt đầu off-screen bên phải, ở slot trên cùng -> reflow sẽ trượt vào.
    t.frame = NSMakeRect(cv.bounds.size.width, 14, sz.width, sz.height);
    __weak MainWindowController *ws = self;
    __weak OS9Toast *wt = t;
    t.onClose = ^{ [ws dismissToast:wt]; };
    [cv addSubview:t positioned:NSWindowAbove relativeTo:nil];
    [_toasts addObject:t];
    while (_toasts.count > 5) {                       // giới hạn stack
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
    NSRect away = t.frame; away.origin.x = _window.contentView.bounds.size.width;  // trượt ra phải + mờ
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = 0.28; t.animator.frame = away; t.animator.alphaValue = 0.0;
    } completionHandler:^{ [t removeFromSuperview]; }];
    [self reflowToasts];   // các toast còn lại trượt xuống lấp chỗ
}

// Xếp toast từ góc phải-TRÊN xuống: mới nhất (cuối mảng) ở trên cùng (content flipped: y nhỏ = trên).
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
