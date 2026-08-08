#import "windows/MainWindowControllerPrivate.h"

#include "core/domain/request/request_defaults.hpp" // defaultPayloadFor (fresh-request factory)

@implementation MainWindowController

#pragma mark Build

- (void)showWindow {
    DeedConfig *cfg = [DeedConfig shared];
    // Display font comes from Settings (app-support) — set BEFORE building widgets.
    { core::AppConfigStore a; a.setDefaults([self appDefaultsFromEnv]); core::AppConfig c = a.load();
      [OS9Theme setConfiguredFontName:N(c.fontName) size:c.fontSize]; }
    // Button style: new (btn-new.svg) by default, or classic (button.svg) via .env.
    [OS9Theme setButtonStyleName:[cfg stringFor:@"BUTTON_STYLE" def:@"new"]];
    NSRect frame = NSMakeRect(0, 0, [cfg floatFor:@"WINDOW_WIDTH" def:1134], [cfg floatFor:@"WINDOW_HEIGHT" def:736]);
    // Window corners (SQUARE_CORNERS): 0 = system titled window (OS-rounded), 1 = borderless SQUARE (OS9
    // default), 2 = borderless + PIXEL-rounded corners (retro, non-AA 9-slice mask). 1 and 2 share the
    // borderless OS9Window; 2 additionally applies _cornerMask after the content view is set (below).
    NSInteger corners = [cfg intFor:@"SQUARE_CORNERS" def:1];
    if (corners == 0) {
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
    } else {
        // + Miniaturizable: allow minimizing into the Dock (genie) while staying borderless.
        _window = [[OS9Window alloc] initWithContentRect:frame
                                               styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable |
                                                          NSWindowStyleMaskMiniaturizable)
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        if (corners == 2) _cornerRadiusPts = [cfg floatFor:@"CORNER_RADIUS_PX" def:6];
    }
    _window.movableByWindowBackground = NO;
    _window.releasedWhenClosed = NO;   // controller owns _window (ARC); don't let close auto-release it
    _window.delegate = self;
    _window.minSize = NSMakeSize([cfg floatFor:@"WINDOW_MIN_WIDTH" def:820], [cfg floatFor:@"WINDOW_MIN_HEIGHT" def:520]);
    // Bright platinum app -> force Aqua so text/fields aren't washed white by system Dark Mode.
    _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];

    OS9BackgroundView *content = [[OS9BackgroundView alloc] initWithFrame:frame];
    _window.contentView = content;
    if (_cornerRadiusPts > 0) [self applyPixelCorners];   // SQUARE_CORNERS=2

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

    [self setRequestType:core::domain::RequestType::Http];
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
    _tree.rowHeight = 20;                  // room for 16px icon + row separator line
    _tree.indentationPerLevel = 14;        // one gutter indent per level -> child triangle under parent label
    _tree.allowsMultipleSelection = YES;   // multi-select to delete at once
    // DISABLE default blue highlight -> draw a light GRAY background ourselves (row view below).
    _tree.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;
    _tree.dataSource = self;
    _tree.delegate = self;
    _tree.target = self;
    _tree.action = @selector(treeClicked:);          // click folder -> fold/unfold
    _tree.doubleAction = @selector(treeDoubleClicked:); // dbl: empty area -> new HTTP; on a row -> rename
    _expandedFolders = [NSMutableSet set];
    _tree.backgroundColor = [NSColor whiteColor];
    [_tree registerForDraggedTypes:@[ kTreeDragType ]];
    // Platinum feedback is drawn by DeedOutlineView; drop AppKit's blue Aqua insertion line.
    _tree.draggingDestinationFeedbackStyle = NSTableViewDraggingDestinationFeedbackStyleNone; // drag-and-drop move
    __weak MainWindowController *weakSelf = self;
    _tree.contextHandler = ^(NSInteger row, NSPoint pt) { [weakSelf showContextMenuForRow:row atWindowPoint:pt]; };
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
}

// Label + body transform per the pretty button's current mode.
- (NSString *)prettyTitle { return @[ StrViewPretty, StrViewRaw, StrViewEncode, StrViewDecode ][_prettyMode]; }
- (NSString *)applyView:(const std::string &)body { return [self applyView:body mode:_prettyMode]; }
// Transform body per an explicit `mode` (reads NO ivars) -> safe to call from a background thread.
- (NSString *)applyView:(const std::string &)body mode:(int)mode {
    switch (mode) {
        case 1: return N(core::serial::formatJson(body, false));
        case 2: return N(core::serial::jsonEncodeString(body));
        case 3: return N(core::serial::jsonDecodeString(body));
        default: return N(core::serial::formatJson(body, true));
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
    _envButton = [[OS9BevelButton alloc] initWithTitle:@"ENV" target:self action:@selector(envClicked:)];
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

    // Kafka client-kind selector: ONLY visible when type=Kafka, sits right before the URL/brokers field;
    // dynamic label reflects the current side (kafkaModeToggled: flips it).
    _kafkaModeToggle = [[OS9Toggle alloc] initWithLabel:StrKafkaProducer target:self action:@selector(kafkaModeToggled:)];

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

    for (NSView *v in @[ _settingButton, _envButton, _sendButton, _cancelButton, _protoPopup, _servicePopup, _methodPopup, _kafkaModeToggle, _urlInset ])
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
    // Settings only: entry point to env editing.
    _manageEnvButton = [[OS9BevelButton alloc] initWithTitle:StrBtnManageEnv target:self
                                                      action:@selector(manageEnvClicked:)];
    [_configPane addSubview:_manageEnvButton];

    _envVC = [[EnvWindowController alloc] initWithEnvRepo:nullptr session:nullptr]; // repos set on open

    // Settings = JSON -> use SciTextView (Scintilla) like the request editor: JSON syntax coloring,
    // line numbers, Platinum theme, OS9 scrollbar. Scintilla manages its own buffer (no AppleSpell spawn).
    // Wrapped in OS9SerratedInset for the serrated border matching the Environments screen (OS9EnvGrid) + other panes.
    _settingInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_configPane addSubview:_settingInset];
    _settingEditor = [[SciTextView alloc] initEditable:YES];
    [_settingInset addSubview:_settingEditor];
}

#pragma mark Conditional render by type

// The current request's protocol. No model -> Http (harmless when no request is open).
- (core::RequestType)requestType {
    return _model ? _model->type() : core::RequestType::Http;
}

// The Kafka payload's client-kind (Producer/Consumer) — nil-safe (Http/... -> Producer, harmless default).
- (core::domain::KafkaClientKind)kafkaClientKind {
    if (!_model || _model->type() != core::domain::RequestType::Kafka) return core::domain::KafkaClientKind::Producer;
    return std::get<core::domain::KafkaRequest>(_model->payload()).kind();
}

// Toolbar Producer/Consumer toggle flipped: archive the OUTGOING kind's kafka-specific buffers + last
// response into _kafka{Producer,Consumer}{Req,Resp}Buffers (mirrors _bodyDrafts) and restore the INCOMING
// kind's archive if one exists, else fall back to a fresh default — a flip must never discard typed
// content. RequestType stays Kafka throughout (pure UI swap).
- (void)kafkaModeToggled:(id)sender {
    if (!_model || _model->type() != core::domain::RequestType::Kafka) return;
    using namespace core::domain;
    const auto &k = std::get<KafkaRequest>(_model->payload());
    bool wasProducer = (k.kind() == KafkaClientKind::Producer);

    // 1) Archive the OUTGOING kind's state. The trailing buffer is the SHARED per-request Config
    //    (timeout_ms/tls) tab — not per-kind — so it's carried over separately, never archived here.
    [self stashActiveReqBuffer];
    NSString *sharedConfigBuf = _reqBuffers.count ? _reqBuffers.lastObject : @"{}";
    NSArray<NSString *> *outgoingKafkaBufs =
        (_reqBuffers.count > 1) ? [_reqBuffers subarrayWithRange:NSMakeRange(0, _reqBuffers.count - 1)] : @[];
    if (wasProducer) {
        _kafkaProducerReqBuffers = outgoingKafkaBufs;
        _kafkaProducerRespBuffers = [_respBuffers copy];
        _kafkaProducerHasResp = _hasResp;
        _kafkaProducerLastResp = _lastResp;
    } else {
        _kafkaConsumerReqBuffers = outgoingKafkaBufs;
        _kafkaConsumerRespBuffers = [_respBuffers copy];
        _kafkaConsumerHasResp = _hasResp;
        _kafkaConsumerLastResp = _lastResp;
    }

    // 2) Resolve the INCOMING kind's kafka-specific buffers: the in-session archive if we have one, else
    //    the model's persisted inactiveDraft (how the other side survives app restart), else a fresh
    //    default — all serialized to JSON text so every path feeds the SAME parse step in (3).
    bool toConsumer = wasProducer;
    NSArray<NSString *> *incomingKafkaBufs = toConsumer ? _kafkaConsumerReqBuffers : _kafkaProducerReqBuffers;
    bool haveDraft = toConsumer ? (incomingKafkaBufs.count >= 1) : (incomingKafkaBufs.count >= 2);
    if (!haveDraft && k.inactiveDraft()) {
        // create()'s invariant: inactiveDraft holds the non-active alternative == the incoming kind.
        if (toConsumer) {
            const auto &c = std::get<KafkaConsumeSpec>(*k.inactiveDraft());
            incomingKafkaBufs = @[ N(core::serial::kafkaConsumeConfigToJson(c.config)) ];
        } else {
            const auto &p = std::get<KafkaProduceSpec>(*k.inactiveDraft());
            incomingKafkaBufs = @[ N(core::serial::kafkaMessageToJson(p.message)),
                                   N(core::serial::kafkaProduceConfigToJson(p.config)) ];
        }
        haveDraft = true;
    }
    if (!haveDraft) {
        if (toConsumer) {
            KafkaConsumeConfig cfg{{KafkaTopic::create("demo-topic").take()}, std::nullopt,
                                  ConsumerGroup::create("").take()};
            incomingKafkaBufs = @[ N(core::serial::kafkaConsumeConfigToJson(cfg)) ];
        } else {
            KafkaProduceConfig cfg{KafkaTopic::create("demo-topic").take()};
            KafkaMessage msg;
            msg.value = MessagePayload{"{}"};
            incomingKafkaBufs = @[ N(core::serial::kafkaMessageToJson(msg)), N(core::serial::kafkaProduceConfigToJson(cfg)) ];
        }
    }

    // 3) Parse those buffers into a domain Mode (same shapes syncModelFromEditors: parses) + rebuild _model.
    // KafkaRequest::Mode has no default alternative -> build the Result inline per branch.
    Result<KafkaRequest::Mode> mode = Result<KafkaRequest::Mode>::fail({ErrorCode::Internal, "", ""});
    if (toConsumer) {
        auto cfgR = core::serial::jsonToKafkaConsumeConfig(S(incomingKafkaBufs[0]));
        if (cfgR) mode = Result<KafkaRequest::Mode>::ok(KafkaRequest::Mode{KafkaConsumeSpec{cfgR.take()}});
    } else {
        auto msgR = core::serial::jsonToKafkaMessage(S(incomingKafkaBufs[0]));
        auto cfgR = core::serial::jsonToKafkaProduceConfig(S(incomingKafkaBufs[1]));
        if (msgR && cfgR) mode = Result<KafkaRequest::Mode>::ok(KafkaRequest::Mode{KafkaProduceSpec{cfgR.take(), msgR.take()}});
    }
    if (!mode.isOk()) return; // archived JSON somehow invalid -> leave the toggle/state untouched

    // The OUTGOING kind rides along as the model's inactiveDraft so it PERSISTS (autosave writes both
    // sides — the in-session buffer archive of step (1) dies with the window). Best effort: parse the
    // just-archived buffers (they may be newer than the model); unparseable mid-edit text falls back to
    // the model's last-synced state of that side.
    std::optional<KafkaRequest::Mode> outgoingDraft{k.mode()};
    if (wasProducer && outgoingKafkaBufs.count >= 2) {
        auto msgR = core::serial::jsonToKafkaMessage(S(outgoingKafkaBufs[0]));
        auto cfgR = core::serial::jsonToKafkaProduceConfig(S(outgoingKafkaBufs[1]));
        if (msgR && cfgR) outgoingDraft = KafkaRequest::Mode{KafkaProduceSpec{cfgR.take(), msgR.take()}};
    } else if (!wasProducer && outgoingKafkaBufs.count >= 1) {
        auto cfgR = core::serial::jsonToKafkaConsumeConfig(S(outgoingKafkaBufs[0]));
        if (cfgR) outgoingDraft = KafkaRequest::Mode{KafkaConsumeSpec{cfgR.take()}};
    }
    KafkaRequest::Mode incomingMode = mode.take();
    auto rebuilt = KafkaRequest::create(k.brokers(), k.security(), incomingMode, std::move(outgoingDraft));
    // A draft that fails create()'s invariants must not block the toggle itself — retry without it.
    if (!rebuilt) rebuilt = KafkaRequest::create(k.brokers(), k.security(), std::move(incomingMode));
    if (!rebuilt) return;
    _model = _model->withPayload(RequestModel::Payload(rebuilt.take()));

    // 4) Recompute tab titles/buttons for the new kind, then populate buffers directly — NOT via
    //    populateEditorsFromModel, which would wipe the drafts just archived in step (1).
    [self setRequestType:RequestType::Kafka];
    NSMutableArray<NSString *> *newReqBufs = [incomingKafkaBufs mutableCopy];
    [newReqBufs addObject:sharedConfigBuf];
    _reqBuffers = newReqBufs;
    NSInteger li = [self tabIndexForKey:_leftPaneActiveTabKey inTitles:_reqTabTitles];
    if (li >= (NSInteger)_reqBuffers.count) li = 0;
    _activeReqTab = li;
    _reqText.string = _reqBuffers.count ? _reqBuffers[li] : @"";
    [self highlightActiveTab:_reqTabButtons active:li];

    // Response pane: restore the incoming kind's cached result (or empty if it was never sent).
    _hasResp = toConsumer ? _kafkaConsumerHasResp : _kafkaProducerHasResp;
    _lastResp = toConsumer ? _kafkaConsumerLastResp : _kafkaProducerLastResp;
    [self applyResponseBuffers:(toConsumer ? _kafkaConsumerRespBuffers : _kafkaProducerRespBuffers) ?: @[]];
    [self updateStatus:@""];
    [self relayout];
}

- (void)mutateGrpc:(void (^)(core::domain::GrpcRequest::Parts &))fn {
    if (!_model || _model->type() != core::domain::RequestType::Grpc) return;
    const auto &g = std::get<core::domain::GrpcRequest>(_model->payload());
    core::domain::GrpcRequest::Parts p;
    p.target = g.target();
    p.service = g.service();
    p.method = g.method();
    p.methodType = g.methodType();
    p.message = g.message();
    p.metadata = g.metadata();
    p.protoSource = g.protoSource();
    p.tls = g.tls();
    fn(p);
    _model = _model->withPayload(core::domain::GrpcRequest::create(std::move(p)).take());
}

- (void)setRequestType:(core::domain::RequestType)t {
    // Ensure _model is of type t. Loading/import pass the model's current type -> no rebuild (keeps data);
    // the initial default + any real switch -> a fresh default payload of the new type.
    if (!_model || _model->type() != t) {
        core::domain::RequestId id = _model ? _model->id() : core::domain::RequestId("");
        std::string nm = _model ? _model->name() : std::string();
        int seq = _model ? _model->seq() : 0;
        // Keep the current request's config across a type switch; for a fresh editor (no model) take the
        // default per-request config from .env (not hardcoded). RequestConfig has no default ctor -> lambda.
        core::domain::RequestConfig cfg = [&]() -> core::domain::RequestConfig {
            if (_model) return _model->config();
            DeedConfig *dc = [DeedConfig shared];
            long long toMs = (long long)[dc intFor:@"DEFAULT_TIMEOUT_MS" def:core::kNewRequestTimeoutMsDefault];
            if (toMs <= 0) toMs = core::kNewRequestTimeoutMsDefault;
            return core::domain::RequestConfig{core::domain::Timeout::fromMillis(toMs).take(),
                                               (bool)[dc boolFor:@"VERIFY_TLS" def:YES]};
        }();
        _model = core::domain::RequestModel::create(id, nm, seq, cfg,
                                                    core::domain::defaultPayloadFor(t))
                     .take();
    }
    // Per-type tab sets come from the binder; Config is the LAST request tab for every type.
    RequestTypeUi *ui = TypeUiFor(t);
    _reqTabTitles = [ui requestTabTitles:*_model];
    _respTabTitles = [ui responseTabTitles:*_model];
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
        // Resign input context before clearing editor/URL contents (avoid dangling context).
        OS9SafeEndEditing(_window, _reqText);
        OS9SafeEndEditing(_window, _respText);
        _reqText.string = @""; _respText.string = @""; _urlField.stringValue = @""; _urlPrevLen = 0;
        _currentRel.clear(); _currentId.clear();
    }
    [self updateTitle];
}

@end
