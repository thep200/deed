#import "windows/MainWindowController.h"

#import "app/DeedConfig.h"
#import "windows/EnvWindowController.h"
#import "theme/OS9Theme.h"
#import "icons/OS9Glyphs.h"
#import "widgets/OS9BackgroundView.h"
#import "widgets/OS9BevelButton.h"
#import "widgets/OS9Divider.h"
#import "widgets/OS9Dropdown.h"
#import "widgets/OS9InsetView.h"
#import "widgets/OS9Label.h"
#import "widgets/OS9PopupButton.h"
#import "widgets/OS9Scroller.h"
#import "widgets/OS9SerratedInset.h"
#import "widgets/OS9TitleBar.h"
#import "widgets/OS9Toast.h"
#import "widgets/OS9Window.h"
#import "editor/SciTextView.h"

#include <memory>
#include <string>

#include "core/engine.hpp"
#include "core/codec/field_codec.hpp"
#include "core/import_export/importer.hpp"
#include "core/persistence/stores.hpp"
#include "core/types.hpp"

#pragma mark - TreeItem (model cho NSOutlineView)

@interface TreeItem : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *relPath;
@property(nonatomic, copy) NSString *requestId;   // id ổn định của request
@property(nonatomic) BOOL isFolder;
@property(nonatomic, copy) NSString *badge;
@property(nonatomic, copy) NSString *mark;   // mốc đầu dòng: HTTP method, hoặc "gRPC"
@property(nonatomic) BOOL grpc;
@property(nonatomic, strong) NSMutableArray<TreeItem *> *children;
@end
@implementation TreeItem
@end

static inline NSString *N(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }
static inline std::string S(NSString *s) { return s ? std::string(s.UTF8String) : std::string(); }

static NSString *const kTreeDragType = @"com.example.deed.request";

// Outline view với context menu động (chuột phải) -> hỏi controller dựng menu.
@interface DeedOutlineView : NSOutlineView
@property(nonatomic, copy) NSMenu *(^menuProvider)(NSInteger clickedRow);
@end
@implementation DeedOutlineView
- (NSMenu *)menuForEvent:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    return self.menuProvider ? self.menuProvider(row) : nil;
}
// Bỏ mũi tên fold (disclosure triangle) — folder luôn mở sẵn.
- (NSRect)frameOfOutlineCellAtRow:(NSInteger)row { return NSZeroRect; }
@end

static TreeItem *BuildTree(const core::TreeNode &n) {
    TreeItem *it = [TreeItem new];
    it.name = N(n.name);
    it.relPath = N(n.relPath);
    it.isFolder = n.isFolder;
    it.requestId = N(n.id);
    it.children = [NSMutableArray array];
    if (!n.isFolder) {
        it.grpc = (n.requestType == core::RequestType::Grpc);
        it.badge = [NSString stringWithFormat:@"%s %s", core::toString(n.requestType).c_str(), n.methodOrType.c_str()];
        // HTTP -> tên method (GET/POST...); gRPC -> "gRPC".
        it.mark = it.grpc ? @"gRPC" : N(n.methodOrType);
    }
    for (const auto &c : n.children) [it.children addObject:BuildTree(c)];
    return it;
}

#pragma mark - TreeCellView (icon thư mục/doc retro tự vẽ)

@interface TreeCellView : NSView
@property(nonatomic, copy) NSString *text;
@property(nonatomic) BOOL isFolder;
@end

@implementation TreeCellView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)d {
    CGFloat h = self.bounds.size.height;
    NSRect icon = NSMakeRect(0, floor((h - 14) / 2), 17, 14);
    if (_isFolder) {
        // Theo folder.svg: THÂN là hình chữ nhật kín + TAB nhỏ nhô lên ở phía trên-trái
        // (thụt vào khỏi mép trái, hai cạnh xiên). Nền trắng, viền đen 1px.
        CGFloat x = icon.origin.x, y = icon.origin.y;   // flipped: y = trên
        CGFloat bt = y + 3;                              // mép trên thân
        NSRect body = NSMakeRect(x, bt, 16, 11);         // thân 16x11
        [[NSColor whiteColor] set];
        NSRectFill(body);
        NSRectFill(NSMakeRect(x, y, 9, 4));              // vùng tab (trắng) từ mép trái
        [[NSColor blackColor] set];
        NSBezierPath *bp = [NSBezierPath bezierPathWithRect:NSMakeRect(x + 0.5, bt + 0.5, 15, 10)];
        bp.lineWidth = 1.0;
        [bp stroke];
        // tab bump: cạnh trái THẲNG ĐỨNG trùng mép trái thân; chỉ cạnh phải xiên.
        NSBezierPath *tab = [NSBezierPath bezierPath];
        [tab moveToPoint:NSMakePoint(x + 0.5, bt)];       // chân trái = mép trái thân
        [tab lineToPoint:NSMakePoint(x + 0.5, y + 1.5)];  // cạnh trái thẳng đứng
        [tab lineToPoint:NSMakePoint(x + 6.5, y + 1.5)];  // đỉnh tab
        [tab lineToPoint:NSMakePoint(x + 8.0, bt)];       // dốc phải xuống mép thân
        tab.lineWidth = 1.0;
        [tab stroke];
    }
    // request: bỏ icon -> text sát lề trái; folder: text sau icon.
    NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont], NSForegroundColorAttributeName : [NSColor blackColor]};
    NSSize sz = [(_text ?: @"") sizeWithAttributes:attrs];
    CGFloat tx = _isFolder ? 18 : 0;
    [(_text ?: @"") drawAtPoint:NSMakePoint(tx, floor((h - sz.height) / 2)) withAttributes:attrs];
}
@end

#pragma mark - MainWindowController

@implementation MainWindowController {
    // Core
    std::unique_ptr<core::Engine> _engine;
    std::unique_ptr<UiDelegateBridge> _bridge;
    core::RequestModel _model;
    std::string _root;
    std::string _currentRel;
    std::string _currentId;   // id request đang mở (định danh ổn định)
    uint64_t _currentHandle;
    BOOL _hasRequest;
    std::vector<core::GrpcMethodInfo> _grpcMethods; // song song với item của _servicePopup
    uint64_t _grpcMethodsReqSeq;  // chống race: chỉ áp kết quả listGrpcMethods mới nhất

    // Chrome + containers
    NSWindow *_window;
    OS9TitleBar *_titleBar;
    NSView *_mainPane;     // màn chính
    NSView *_configPane;   // màn cấu hình (ENV + Setting)
    BOOL _configMode;

    // (1)(2) tree
    OS9BevelButton *_openButton;
    OS9SerratedInset *_treeInset;
    NSScrollView *_treeScroll;
    DeedOutlineView *_tree;
    NSTextField *_renameField;   // ô rename inline trên cây (overlay)
    TreeItem *_renameItem;
    NSMutableArray<TreeItem *> *_roots;
    NSMutableSet<NSString *> *_expandedFolders; // relPath các folder đang mở (giữ qua reload)
    BOOL _treeExpandInit;                        // lần nạp đầu -> mở hết

    // (3) request editor
    OS9SerratedInset *_reqInset;
    SciTextView *_reqText;   // editor request (Scintilla)
    NSMutableArray<NSString *> *_reqBuffers;
    NSMutableArray<OS9BevelButton *> *_reqTabButtons;
    NSArray<NSString *> *_reqTabTitles;
    NSInteger _activeReqTab;

    // (4) response
    OS9SerratedInset *_respInset;
    SciTextView *_respText;  // editor response (Scintilla, read-only)
    NSMutableArray<NSString *> *_respBuffers;
    NSMutableArray<OS9BevelButton *> *_respTabButtons;
    NSArray<NSString *> *_respTabTitles;
    NSInteger _activeRespTab;
    OS9BevelButton *_prettyButton;
    NSInteger _prettyMode; // 0=Pretty 1=Raw 2=Encode 3=Decode
    OS9BevelButton *_curlButton;      // copy request hiện tại as cURL
    core::ApiResponse _lastResp;
    BOOL _hasResp;

    // status line (trên panes, dưới tab buttons)
    OS9SerratedInset *_statusBar;
    NSTextField *_statusLabel;

    // toolbar
    OS9BevelButton *_settingButton;
    OS9BevelButton *_envButton;
    OS9BevelButton *_sendButton;
    OS9BevelButton *_cancelButton;
    OS9PopupButton *_protoPopup;     // gRPC: chọn nguồn proto (Reflection | .proto)
    OS9PopupButton *_servicePopup;   // gRPC: chọn service/RPC (trước nút Send)
    OS9PopupButton *_methodPopup;
    OS9SerratedInset *_urlInset; // khung input răng cưa retro bọc ô URL
    NSTextField *_urlField;

    // dividers + bề rộng pane
    OS9Divider *_divTree;
    OS9Divider *_divResp;
    CGFloat _treeW;
    CGFloat _reqW;
    NSRect _preZoomFrame; // lưu frame trước khi phóng to (để thu nhỏ lại)

    // toast (stack góc phải-dưới, đẩy lên)
    NSMutableArray<OS9Toast *> *_toasts;

    // config screen (2 màn riêng: Environments / Settings)
    OS9BevelButton *_backButton;
    NSTextField *_configTitle;
    NSInteger _configKind; // 0 = Environments, 1 = Settings
    EnvWindowController *_envVC;
    NSScrollView *_settingScroll;
    NSTextView *_settingText;

    BOOL _sending;
    NSTimer *_spinTimer;   // animate icon loading trong nút Send
    CGFloat _spinPhase;
    NSUInteger _urlPrevLen; // độ dài URL lần trước -> nhận biết "dán" (tăng đột biến)
    NSTextView *_fieldEditor; // field editor dùng chung, tắt hết auto-features
    NSString *_bodyMode;    // mode body hiện tại: json | binary(file) | form-urlencoded(form)
}

#pragma mark Build

- (void)showWindow {
    DeedConfig *cfg = [DeedConfig shared];
    // Font hiển thị lấy từ Settings (app-support) — set TRƯỚC khi dựng widget.
    { core::AppConfigStore a; core::AppConfig c = a.load();
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
    _curlButton.toolTip = @"Copy request hiện tại dạng cURL";
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
    _protoPopup.toolTip = @"Nguồn proto: Reflection (hỏi server) hoặc nạp file .proto";

    // gRPC: chọn service/RPC mà server cung cấp (đặt trước nút Send).
    _servicePopup = [[OS9PopupButton alloc] initWithItems:@[ @"No rpc" ]
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

    _settingScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _settingScroll.hasVerticalScroller = YES;
    _settingScroll.borderType = NSBezelBorder;
    [self styleScroller:_settingScroll];
    _settingText = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    _settingText.font = [OS9Theme monoFont];
    _settingText.richText = NO;
    // Tắt mọi tính năng tự động macOS (spell/autofill/completion/substitution) -> không
    // spawn tiến trình con hỗ trợ (AppleSpell, text-completion…).
    [self disableAutoFeatures:_settingText];
    _settingText.verticallyResizable = YES;
    _settingText.horizontallyResizable = NO;
    _settingText.textContainer.widthTracksTextView = YES;
    _settingScroll.documentView = _settingText;
    [_configPane addSubview:_settingScroll];
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
        _settingScroll.frame = body;
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
    _prettyButton.toolTip = @"Pretty/Raw/Encode/Decode — áp cho pane đang có con trỏ";
    [_mainPane addSubview:_prettyButton];
    _activeReqTab = 0;
    _activeRespTab = 0;
}

- (void)setHasRequest:(BOOL)has {
    _hasRequest = has;
    _reqText.editable = has;
    _sendButton.enabledState = has && !_sending;
    if (!has) {
        _reqText.string = @""; _respText.string = @""; _urlField.stringValue = @""; _urlPrevLen = 0;
        _currentRel.clear(); _currentId.clear();
    }
    [self updateTitle];
}

#pragma mark Collection / tree

- (void)openFolder:(id)sender {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES; p.canChooseFiles = NO; p.allowsMultipleSelection = NO;
    p.prompt = @"Open Collection";
    if ([p runModal] == NSModalResponseOK) [self openCollectionRoot:p.URL.path];
}

- (void)openCollectionRoot:(NSString *)path {
    [self autosaveCurrent];
    [_expandedFolders removeAllObjects];   // collection mới: reset trạng thái fold
    _treeExpandInit = NO;
    _root = path.UTF8String;
    core::EngineConfig cfg; cfg.collectionRoot = _root;
    _engine = std::make_unique<core::Engine>(cfg);
    _bridge = std::make_unique<UiDelegateBridge>(self);
    _envVC = [[EnvWindowController alloc] initWithEngine:_engine.get()];
    // Ghi nhớ thư mục này vào app-support để lần sau mở lại đúng nó.
    try { core::AppConfig ac = _engine->appConfig().load(); ac.lastCollectionRoot = _root;
          _engine->appConfig().save(ac); } catch (...) {}
    _openButton.title = [self abbreviatePath:path];
    _openButton.toolTip = path;
    [self setHasRequest:NO];
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

- (void)reloadTree {
    [_roots removeAllObjects];
    if (_engine) {
        try {
            core::TreeNode root = _engine->collection().scanTree();
            for (const auto &c : root.children) [_roots addObject:BuildTree(c)];
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    }
    [_tree reloadData];
    [self applyTreeExpansion];
}

// Mở/thu folder theo trạng thái đã lưu; lần đầu mở hết.
- (void)applyTreeExpansion {
    if (!_treeExpandInit) {
        for (TreeItem *r in _roots) [_tree expandItem:r expandChildren:YES];
        [self collectAllFolders:_roots];
        _treeExpandInit = YES;
        return;
    }
    [self restoreExpansion:_roots];
}
- (void)collectAllFolders:(NSArray<TreeItem *> *)items {
    for (TreeItem *t in items)
        if (t.isFolder) { [_expandedFolders addObject:t.relPath]; [self collectAllFolders:t.children]; }
}
- (void)restoreExpansion:(NSArray<TreeItem *> *)items {
    for (TreeItem *t in items) {
        if (!t.isFolder) continue;
        if ([_expandedFolders containsObject:t.relPath]) [_tree expandItem:t];
        [self restoreExpansion:t.children];
    }
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

// Double-click: vùng TRỐNG -> tạo nhanh HTTP request; trên 1 row -> rename inline.
- (void)treeDoubleClicked:(id)sender {
    NSInteger row = _tree.clickedRow;
    if (row < 0) { [self newHttp:nil]; return; }   // khoảng trống
    [self beginInlineRenameRow:row];
}

#pragma mark Inline rename (sửa tên ngay trên cây, không popup)

- (void)beginInlineRenameRow:(NSInteger)row {
    if (row < 0 || !_engine) return;
    TreeItem *t = [_tree itemAtRow:row];
    if (!t || t.relPath.length == 0) return;
    [self commitInlineRename:nil];   // đóng ô đang mở (nếu có)
    // Đặt ô rename trên contentView (không nhét vào NSOutlineView vì nó tự quản subview).
    NSView *content = _window.contentView;
    NSRect cell = [_tree frameOfCellAtColumn:0 row:row];
    NSRect inWin = [_tree convertRect:cell toView:content];
    CGFloat tx = inWin.origin.x + 20;  // chừa icon thư mục/doc
    NSRect fr = NSMakeRect(tx, inWin.origin.y, NSMaxX(inWin) - tx - 2, inWin.size.height);
    _renameField = [[NSTextField alloc] initWithFrame:fr];
    _renameField.stringValue = t.name;
    _renameField.font = [OS9Theme uiFont];
    _renameField.bezeled = YES;
    _renameField.editable = YES;
    _renameField.drawsBackground = YES;
    _renameField.focusRingType = NSFocusRingTypeNone;
    _renameField.target = self;
    _renameField.action = @selector(commitInlineRename:);  // Enter -> commit
    _renameField.delegate = self;                          // blur -> commit (controlTextDidEndEditing)
    _renameItem = t;
    [content addSubview:_renameField positioned:NSWindowAbove relativeTo:nil];
    [_window makeFirstResponder:_renameField];
    [_renameField selectText:nil];
}

// Commit cả khi Enter lẫn khi mất focus. Nil _renameField TRƯỚC để tránh re-entry.
- (void)commitInlineRename:(id)sender {
    NSTextField *f = _renameField;
    TreeItem *t = _renameItem;
    if (!f || !t) return;
    _renameField = nil; _renameItem = nil;
    NSString *newName = [f.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    [f removeFromSuperview];
    if (!newName.length || [newName isEqualToString:t.name]) return;   // không đổi
    [self autosaveCurrent];
    BOOL wasCurrent = (!_currentId.empty() && t.requestId.length && S(t.requestId) == _currentId);
    try {
        std::string newRel = _engine->collection().rename(t.relPath.UTF8String, newName.UTF8String);
        if (wasCurrent) { _currentRel = newRel; [self updateTitle]; } // giữ con trỏ request đang mở
        [self reloadTree];
        [self toastOk:@"Renamed"];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)outlineViewItemDidExpand:(NSNotification *)n {
    TreeItem *t = n.userInfo[@"NSObject"];
    if (t.relPath) [_expandedFolders addObject:t.relPath];
}
- (void)outlineViewItemDidCollapse:(NSNotification *)n {
    TreeItem *t = n.userInfo[@"NSObject"];
    if (t.relPath) [_expandedFolders removeObject:t.relPath];
}

- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    return item == nil ? _roots.count : ((TreeItem *)item).children.count;
}
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)idx ofItem:(id)item {
    return item == nil ? _roots[idx] : ((TreeItem *)item).children[idx];
}
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item { return ((TreeItem *)item).isFolder; }
- (NSView *)outlineView:(NSOutlineView *)ov viewForTableColumn:(NSTableColumn *)col item:(id)item {
    TreeItem *t = item;
    TreeCellView *cell = [ov makeViewWithIdentifier:@"treecell" owner:self];
    if (!cell) {
        cell = [[TreeCellView alloc] initWithFrame:NSMakeRect(0, 0, col.width, 18)];
        cell.identifier = @"treecell";
        cell.translatesAutoresizingMaskIntoConstraints = YES;
    }
    cell.isFolder = t.isFolder;
    cell.text = t.isFolder ? t.name : [NSString stringWithFormat:@"%@  %@", t.mark ?: @"", t.name];
    [cell setNeedsDisplay:YES];
    return cell;
}
- (void)outlineViewSelectionDidChange:(NSNotification *)note {
    if (_tree.selectedRowIndexes.count != 1) return; // multi-select -> không auto-load
    NSInteger row = _tree.selectedRow;
    if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    if (t.isFolder || t.relPath.length == 0) return;
    [self loadRequestAtRel:t.relPath];
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
    for (NSPasteboardItem *pb in [[info draggingPasteboard] pasteboardItems]) {
        NSString *src = [pb stringForType:kTreeDragType];
        if (!src.length) continue;
        try { _engine->collection().move(src.UTF8String, destFolder); any = YES; }
        catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
    }
    if (any) [self reloadTree];
    return any;
}

#pragma mark Load / populate / sync

- (void)loadRequestAtRel:(NSString *)rel {
    if (!_engine) return;
    if (_hasRequest && S(rel) != _currentRel) [self autosaveCurrent]; // tự lưu trước khi chuyển
    try {
        _model = _engine->collection().loadRequest(rel.UTF8String);
        _currentRel = rel.UTF8String;
        _currentId = _model.id;          // theo dõi request đang mở bằng id ổn định
        _hasRequest = YES;
        _hasResp = NO;
        [self setRequestType:_model.type];
        [self populateEditorsFromModel];
        [self setHasRequest:YES];
        _engine->session().saveLastOpened(rel.UTF8String);
        [self updateTitle];
        [self relayout];
        [self updateStatus:@""];
        _respText.string = @"";
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

- (void)populateEditorsFromModel {
    [_reqBuffers removeAllObjects];
    using namespace core;
    if (_model.type == RequestType::Http) {
        HttpRequest &h = _model.http;
        // Body mặc định JSON: request chưa có body (mode none) -> hiển thị như JSON.
        if (h.body.mode == "none") { h.body = Body{}; h.body.mode = "json"; }
        // Buffer body = ĐÚNG nội dung muốn gửi (không bọc {"mode","json"}); mode do app giữ.
        [_reqBuffers addObject:[self bodyBufferFromModel:h.body]];     // 0 = Body (ngoài cùng trái)
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
        [self showSavedGrpcMethodLabel];   // hiện RPC đã lưu (KHÔNG fetch; fetch khi bấm dropdown)
    }
    _activeReqTab = 0;
    _reqText.string = _reqBuffers.count ? _reqBuffers[0] : @"";
    [self highlightActiveTab:_reqTabButtons active:0];
}

- (void)stashActiveReqBuffer {
    // [copy] BẮT BUỘC: NSTextView.string trả tham chiếu tới text storage SỐNG;
    // không copy -> buffer các tab cùng trỏ 1 chuỗi đang đổi -> giá trị lẫn vào nhau.
    if (_activeReqTab >= 0 && _activeReqTab < (NSInteger)_reqBuffers.count)
        _reqBuffers[_activeReqTab] = [(_reqText.string ?: @"") copy];
}

// Trả NO nếu JSON sai (báo toast + chọn tab). silent=YES -> không đổi tab/không toast (autosave).
- (BOOL)syncModelFromEditors:(BOOL)silent {
    [self stashActiveReqBuffer];
    using namespace core;
    std::string err;
    NSArray<NSString *> *names = _reqTabTitles;
    auto fail = [&](NSInteger tab, const std::string &e) {
        if (!silent) {
            [self selectReqTab:tab];
            NSString *tn = (tab >= 0 && tab < (NSInteger)names.count) ? names[tab] : @"?";
            [self toastWarn:[NSString stringWithFormat:@"Invalid JSON in tab %@: %s", tn, e.c_str()]];
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

// Nếu request đang mở nằm trong danh sách item bị xoá -> ĐÓNG editor để autosave KHÔNG tạo lại file.
// So khớp theo id ổn định (fallback relPath).
- (void)closeEditorIfDeleted:(NSArray<TreeItem *> *)deleted {
    for (TreeItem *t in deleted) {
        BOOL match = (_currentId.size() && t.requestId.length && S(t.requestId) == _currentId) ||
                     (S(t.relPath) == _currentRel && !_currentRel.empty());
        if (match) { _currentRel.clear(); _currentId.clear(); [self setHasRequest:NO]; return; }
    }
}

// Tự lưu mọi thay đổi (không hỏi). JSON sai -> bỏ qua + cảnh báo nhẹ.
- (void)autosaveCurrent {
    if (!_hasRequest || !_engine || _currentRel.empty()) return;
    if (![self resyncCurrentRelById]) return;     // request đã bị xoá/đổi path -> không ghi lại path cũ
    if (![self syncModelFromEditors:YES]) { [self toastWarn:@"Autosave failed: invalid JSON"]; return; }
    try { _engine->collection().saveRequest(_currentRel, _model); [_tree reloadData];
          [self applyTreeExpansion]; }
    catch (...) {}
}

#pragma mark Tabs

#pragma mark Body dropdown (json/file/form)

// Tên hiển thị nút Body theo mode hiện tại: "Body (JSON)" / "Body (FILE)" / "Body (FORM)".
- (NSString *)bodyButtonTitle {
    NSString *m = _bodyMode.length ? _bodyMode : @"json";
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:m]) return d[@"label"];
    return @"JSON";   // text/xml/none -> JSON (giữ hành vi cũ)
}
- (NSInteger)bodyTabIndex { return [_reqTabTitles indexOfObject:@"Body"]; } // 0 cho HTTP, NSNotFound cho gRPC
- (void)updateBodyButtonLabel {
    NSInteger bi = [self bodyTabIndex];
    if (bi == NSNotFound || bi >= (NSInteger)_reqTabButtons.count) return;
    _reqTabButtons[bi].title = [self bodyButtonTitle];
}
// Template body cho từng mode — KHÔNG bọc key "mode" (app tự giữ mode), nhưng CÓ sẵn
// các key gợi ý để người dùng biết điền gì.
//   json -> JSON thô.   form -> 1 entry key/value mẫu.   file -> object có key filePath.
static NSString *const kFormBodyTemplate =
    @"[\n  {\n    \"key\": \"\",\n    \"value\": \"\",\n    \"enabled\": true\n  }\n]";
static NSString *const kFileBodyTemplate = @"{\n  \"filePath\": \"\"\n}";

// NGUỒN DUY NHẤT cho dropdown Body: mode nội bộ <-> option/label/template.
// Thêm/bớt định dạng chỉ cần sửa bảng này (trước đây rải rác ở 3 hàm if-else).
static NSArray<NSDictionary *> *BodyModeTable(void) {
    static NSArray *t;
    if (!t) t = @[
        @{@"mode" : @"json",            @"opt" : @"JSON", @"label" : @"JSON", @"tpl" : @"{}"},
        @{@"mode" : @"binary",          @"opt" : @"File", @"label" : @"File", @"tpl" : kFileBodyTemplate},
        @{@"mode" : @"form-urlencoded", @"opt" : @"Form", @"label" : @"Form", @"tpl" : kFormBodyTemplate},
    ];
    return t;
}
- (NSString *)bodyTemplateForMode:(NSString *)mode {
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:mode]) return d[@"tpl"];
    return @"{}";                                        // json (mặc định cho text/xml/none)
}

// Model.Body -> nội dung hiển thị trong editor (đúng cái gửi đi, không bọc).
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
    return N(b.json.empty() ? "{}" : b.json);            // json (mặc định)
}

// Editor buffer -> Model.Body, parse theo _bodyMode (mode do app định nghĩa, không nằm trong text).
- (BOOL)syncBodyFromBuffer:(NSString *)buf into:(core::Body &)out err:(std::string &)err {
    using namespace core;
    NSString *bm = _bodyMode.length ? _bodyMode : @"json";
    Body nb;
    if ([bm isEqualToString:@"form-urlencoded"]) {
        nb.mode = "form-urlencoded";
        if (!fieldcodec::jsonToKeyValues(S(buf), nb.formUrlEncoded, err)) return NO;
    } else if ([bm isEqualToString:@"binary"]) {
        nb.mode = "binary";
        // Nhận cả object {"filePath": "..."} lẫn chuỗi path thuần (tương thích cũ).
        NSString *t = [buf stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        id obj = t.length ? [NSJSONSerialization JSONObjectWithData:[t dataUsingEncoding:NSUTF8StringEncoding]
                                                            options:0 error:nil] : nil;
        if ([obj isKindOfClass:[NSDictionary class]]) {
            NSString *fp = ((NSDictionary *)obj)[@"filePath"] ?: ((NSDictionary *)obj)[@"path"];
            nb.binaryFilePath = fp.length ? S(fp) : "";
        } else {
            nb.binaryFilePath = S(t);   // coi cả text là path
        }
    } else if ([bm isEqualToString:@"text"]) {
        nb.mode = "text"; nb.text = S(buf);
    } else if ([bm isEqualToString:@"xml"]) {
        nb.mode = "xml"; nb.xml = S(buf);
    } else {
        nb.mode = "json"; nb.json = S(buf);   // JSON thô người dùng nhập, KHÔNG encode vào key "json"
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
    [self stashActiveReqBuffer];                 // lưu nội dung đang gõ vào tab hiện tại
    if (![_bodyMode isEqualToString:mode])       // đổi mode -> nạp template mới
        _reqBuffers[bi] = [self bodyTemplateForMode:mode];
    _bodyMode = mode;
    [self updateBodyButtonLabel];
    _activeReqTab = bi;                          // kích hoạt + hiện body
    _reqText.string = _reqBuffers[bi];
    [self highlightActiveTab:_reqTabButtons active:bi];
}

#pragma mark Tabs

- (void)reqTabClicked:(OS9BevelButton *)b { [self selectReqTab:b.tag]; }
- (void)selectReqTab:(NSInteger)tab {
    if (tab < 0 || tab >= (NSInteger)_reqBuffers.count) return;
    [self stashActiveReqBuffer];
    _activeReqTab = tab;
    _reqText.string = _reqBuffers[tab];
    [self highlightActiveTab:_reqTabButtons active:tab];
}
- (void)respTabClicked:(OS9BevelButton *)b {
    NSInteger tab = b.tag;
    if (tab < 0 || tab >= (NSInteger)_respBuffers.count) return;
    _activeRespTab = tab;
    _respText.string = _respBuffers[tab];
    [self highlightActiveTab:_respTabButtons active:tab];
}
- (void)highlightActiveTab:(NSArray<OS9BevelButton *> *)buttons active:(NSInteger)active {
    for (OS9BevelButton *b in buttons) b.isDefault = (b.tag == active);
}
- (void)prettyToggle:(id)sender {
    _prettyMode = (_prettyMode + 1) % 4;   // Pretty -> Raw -> Encode -> Decode -> ...
    _prettyButton.title = [self prettyTitle];
    [self applyPrettyToFocusedPane];
}

// Áp chế độ hiện tại lên Ô ĐANG CÓ CON TRỎ: editor request (tab đang mở), setting,
// hoặc (mặc định) pane response. Nút bevel không nhận focus nên firstResponder giữ nguyên.
- (void)applyPrettyToFocusedPane {
    // request editor (Scintilla) đang giữ con trỏ?
    if ([_reqText hasFocus]) {
        _reqText.string = [self applyView:S(_reqText.string)];
        [self stashActiveReqBuffer];
        return;
    }
    // setting editor (NSTextView) đang focus?
    id fr = _window.firstResponder;
    if (fr == _settingText) {
        _settingText.string = [self applyView:S(_settingText.string)];
        return;
    }
    if (_hasResp) [self rebuildResponseBuffers]; // mặc định: pane response
}

// Copy request hiện tại dạng cURL (HTTP) / grpcurl (gRPC) vào clipboard.
- (void)copyAsCurl:(id)sender {
    if (!_hasRequest || !_engine) return;
    if (![self syncModelFromEditors:NO]) return;
    core::ResolvedRequest rr = _engine->resolveRequest(_model);
    std::string curl = core::toCurl(rr.model);
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:N(curl) forType:NSPasteboardTypeString];
    [self toastOk:@"Copied as cURL"];
}

// Zoom toggle thủ công (performZoom đôi khi không thu nhỏ lại được).
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

// Minimize: thu cửa sổ vào Dock (genie). Window borderless + Miniaturizable -> miniaturize: chạy.
- (void)collapseToggle:(id)sender { [_window miniaturize:nil]; }

// Áp font cấu hình (từ Settings) cho mọi ô chữ + vẽ lại.
- (void)applyConfiguredFontAndRefresh {
    core::AppConfigStore a; core::AppConfig c = a.load();
    [OS9Theme setConfiguredFontName:N(c.fontName) size:c.fontSize];
    NSFont *mono = [OS9Theme monoFont];
    [_reqText setFontName:N(c.fontName) size:c.fontSize];
    [_respText setFontName:N(c.fontName) size:c.fontSize];
    _settingText.font = mono; _urlField.font = mono;
    _tree.font = [OS9Theme uiFont];
    [_tree reloadData];
    [self applyTreeExpansion];
    [_window.contentView setNeedsDisplay:YES];
}

#pragma mark Editing

// Dán cURL/grpcurl vào ô URL -> tự nhận biết -> preview -> tạo request mới (CURL_IMPORT.md).
// Nhận biết "dán/drop" bằng độ dài tăng đột biến (>=8 ký tự một lần) — gõ tay tăng 1/ký tự.
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object != _urlField) return;
    NSString *text = _urlField.stringValue ?: @"";
    NSUInteger len = text.length;
    NSUInteger prev = _urlPrevLen;
    _urlPrevLen = len;
    if (!_engine || len < prev + 8) return;            // không phải dán -> bỏ qua
    BOOL isCurl = _engine->looksLikeCurl(text.UTF8String);
    BOOL isGrpc = !isCurl && _engine->looksLikeGrpcurl(text.UTF8String);
    if (!isCurl && !isGrpc) return;
    // cURL: tự import + tạo request luôn (chỉ toast, KHÔNG popup). grpcurl: vẫn xác nhận popup.
    // Defer: tránh xử lý NGAY trong callback đổi text (field editor đang bận).
    dispatch_async(dispatch_get_main_queue(), ^{
        if (isCurl) [self importNow:text grpc:NO];
        else        [self offerImport:text grpc:YES];
    });
}

// Import + tạo request ngay, không hỏi; báo kết quả qua toast.
- (void)importNow:(NSString *)text grpc:(BOOL)isGrpc {
    core::ImportResult r = isGrpc ? _engine->importFromGrpc(text.UTF8String)
                                  : _engine->importFromCurl(text.UTF8String);
    if (!r.ok) {
        [self toastWarn:[NSString stringWithFormat:@"Import %@ failed: %s",
                         isGrpc ? @"grpcurl" : @"cURL", r.error.c_str()]];
        [self restoreUrlField];
        return;
    }
    [self applyImport:r.model];
}

// Hiện preview xác nhận; nếu OK -> tạo request mới trong tree + mở editor.
- (void)offerImport:(NSString *)text grpc:(BOOL)isGrpc {
    core::ImportResult r = isGrpc ? _engine->importFromGrpc(text.UTF8String)
                                  : _engine->importFromCurl(text.UTF8String);
    if (!r.ok) {
        [self toastWarn:[NSString stringWithFormat:@"Import %@ failed: %s",
                         isGrpc ? @"grpcurl" : @"cURL", r.error.c_str()]];
        return;
    }
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = isGrpc ? @"grpcurl command detected" : @"cURL command detected";
    a.informativeText = [self importSummary:r.model unknown:r.unknown grpc:isGrpc];
    [a addButtonWithTitle:(_hasRequest ? @"Replace current" : @"Create request")];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] == NSAlertFirstButtonReturn) {
        [self applyImport:r.model];
    } else {
        [self restoreUrlField];   // bỏ: trả ô URL về giá trị request đang mở
    }
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

// Tên gợi ý: HTTP "METHOD lastPathSegment"; gRPC = method.
- (NSString *)deriveImportName:(const core::RequestModel &)m {
    if (m.type == core::RequestType::Grpc)
        return m.grpc.method.empty() ? @"Imported gRPC" : N(m.grpc.method);
    NSString *url = N(m.http.url);
    NSString *path = url;
    NSRange q = [path rangeOfString:@"?"]; if (q.location != NSNotFound) path = [path substringToIndex:q.location];
    NSString *last = path.lastPathComponent;
    if (!last.length || [last containsString:@":"]) last = @"request"; // chỉ có host
    return [NSString stringWithFormat:@"%s %@", m.http.method.c_str(), last];
}

// REPLACE request đang mở bằng model import (giữ id/name/file, thay type + payload).
// Không có request nào đang mở -> tạo mới (fallback).
- (void)applyImport:(const core::RequestModel &)m {
    if (!_hasRequest || _currentRel.empty() || ![self resyncCurrentRelById]) {
        NSString *name = [self deriveImportName:m];   // fallback: chưa mở request -> tạo mới
        try {
            std::string rel = _engine->collection().createRequestFromModel([self selectedFolderRel], m, name.UTF8String);
            [self reloadTree];
            [self loadRequestAtRel:N(rel)];
            [self toastOk:[NSString stringWithFormat:@"Imported & created: %@", name]];
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
        return;
    }
    // Replace tại chỗ: giữ id + name của request hiện tại; URL/target hiện giá trị đã parse.
    core::RequestModel n = m;
    n.id = _model.id;
    n.name = _model.name;
    _model = n;
    _hasResp = NO;
    [self setRequestType:_model.type];   // rebuild tab theo type mới (http <-> grpc)
    [self populateEditorsFromModel];
    [self setHasRequest:YES];
    _respText.string = @"";              // xoá response cũ
    [self updateTitle];
    [self relayout];
    try {
        _engine->collection().saveRequest(_currentRel, _model);   // lưu ngay tại chỗ
        [self reloadTree];
        [self toastOk:[NSString stringWithFormat:@"Replaced current request (%@)",
                       _model.type == core::RequestType::Grpc ? @"gRPC" : @"HTTP"]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// Trả ô URL về URL/target của request đang mở (tránh lưu nhầm text lệnh).
- (void)restoreUrlField {
    if (!_hasRequest) { _urlField.stringValue = @""; _urlPrevLen = 0; return; }
    NSString *u = (_model.type == core::RequestType::Grpc) ? N(_model.grpc.target) : N(_model.http.url);
    _urlField.stringValue = u;
    _urlPrevLen = u.length;
}

- (void)controlTextDidEndEditing:(NSNotification *)note {
    if (note.object == _renameField) [self commitInlineRename:nil];   // rời ô rename -> lưu
}
- (void)methodChanged:(id)sender { }
- (void)urlCommitted:(id)sender {
    // gRPC: ô URL = target -> Enter nạp lại danh sách RPC. HTTP: tách query.
    if (_model.type == core::RequestType::Grpc) [self reloadGrpcMethods];
    else [self parseUrlQueryIntoQueryTab];
}

// Decode 1 thành phần query: '+' -> space, %XX -> byte (khớp Core urlutil::urlDecode).
- (NSString *)urlDecodeComponent:(NSString *)s {
    NSString *plus = [s stringByReplacingOccurrencesOfString:@"+" withString:@" "];
    return [plus stringByRemovingPercentEncoding] ?: plus;
}

// Nếu ô URL có '?...': tách query (decode) -> nối vào tab Query, ô URL còn raw.
// Dùng khi user tự gõ query vào URL rồi Enter/Send (giống hành vi import cURL).
- (void)parseUrlQueryIntoQueryTab {
    NSInteger qi = [_reqTabTitles indexOfObject:@"Query"];
    if (qi == NSNotFound || qi >= (NSInteger)_reqBuffers.count) return;   // gRPC: không có Query
    NSString *u = _urlField.stringValue ?: @"";
    NSRange qr = [u rangeOfString:@"?"];
    if (qr.location == NSNotFound) return;                                // không có query
    NSString *raw = [u substringToIndex:qr.location];
    NSString *query = [u substringFromIndex:qr.location + 1];
    NSRange hr = [query rangeOfString:@"#"];
    if (hr.location != NSNotFound) query = [query substringToIndex:hr.location];

    [self stashActiveReqBuffer];   // buffer tab Query hiện tại là mới nhất trước khi nối thêm

    // Lấy các entry sẵn có trong tab Query (JSON array) rồi nối entry parse được.
    NSMutableArray *items = [NSMutableArray array];
    NSData *cur = [(_reqBuffers[qi] ?: @"[]") dataUsingEncoding:NSUTF8StringEncoding];
    id arr = cur ? [NSJSONSerialization JSONObjectWithData:cur options:0 error:nil] : nil;
    if ([arr isKindOfClass:[NSArray class]]) [items addObjectsFromArray:arr];
    for (NSString *seg in [query componentsSeparatedByString:@"&"]) {
        if (!seg.length) continue;
        NSRange eq = [seg rangeOfString:@"="];
        NSString *k = (eq.location == NSNotFound) ? seg : [seg substringToIndex:eq.location];
        NSString *v = (eq.location == NSNotFound) ? @"" : [seg substringFromIndex:eq.location + 1];
        [items addObject:@{@"key" : [self urlDecodeComponent:k],
                           @"value" : [self urlDecodeComponent:v], @"enabled" : @YES}];
    }
    NSData *out = [NSJSONSerialization dataWithJSONObject:items options:NSJSONWritingPrettyPrinted error:nil];
    if (out) _reqBuffers[qi] = [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding];

    _urlField.stringValue = raw; _urlPrevLen = raw.length;       // ô URL còn raw
    if (_activeReqTab == qi) _reqText.string = _reqBuffers[qi];   // đang xem tab Query -> refresh
}

- (void)updateTitle {
    // Title CHỈ là tên request (rỗng nếu chưa chọn). Không còn dấu dirty.
    _titleBar.title = _hasRequest ? N(_model.name) : @"";
    [_titleBar setNeedsDisplay:YES];
}

#pragma mark Save (thủ công vẫn giữ ⌘S)

- (void)saveRequest:(id)sender {
    if (!_hasRequest || !_engine) return;
    if (![self resyncCurrentRelById]) { [self toastWarn:@"Request no longer exists"]; return; }
    if (![self syncModelFromEditors:NO]) return;
    try {
        _engine->collection().saveRequest(_currentRel, _model);
        [self reloadTree];
        [self toastOk:@"Saved"];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

#pragma mark Send / Cancel

- (void)sendRequest:(id)sender {
    if (!_hasRequest || !_engine || _sending) return;
    [self parseUrlQueryIntoQueryTab];   // user gõ query vào URL -> tách vào tab Query trước khi sync
    if (![self syncModelFromEditors:NO]) return;
    if (_model.type == core::RequestType::Grpc && _model.grpc.methodType != "unary") {
        [self toastWarn:@"POC supports unary gRPC only"]; return;
    }
    _sending = YES;
    [self startSendSpinner];           // icon loading quay thay cho label
    _cancelButton.enabledState = YES;
    [self relayout];
    [self updateStatus:@""];
    _currentHandle = _engine->send(_model, _bridge.get());
}

- (void)cancelClicked:(id)sender { if (_sending && _engine) _engine->cancel(_currentHandle); }

- (void)onCoreResponse:(uint64_t)handle response:(const core::ApiResponse &)resp {
    if (handle != _currentHandle) return;
    NSLog(@"[smoke] onCoreResponse status=%d bytes=%lld", resp.statusCode, (long long)resp.sizeBytes);
    [self finishSending];
    _lastResp = resp; _hasResp = YES;
    [self rebuildResponseBuffers];
    [self updateStatusFromResponse:resp error:NO];
}

- (void)onCoreError:(uint64_t)handle error:(const core::ApiError &)err {
    if (handle != _currentHandle) return;
    NSLog(@"[smoke] onCoreError kind=%s msg=%s", core::toString(err.kind).c_str(), err.message.c_str());
    [self finishSending];
    NSString *kind = N(core::toString(err.kind));
    _statusLabel.stringValue = (err.kind == core::ErrorKind::Cancelled) ? @"Cancelled" : [NSString stringWithFormat:@"✕ %@", kind];
    _statusLabel.textColor = [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0];
    _hasResp = NO;
    [_respBuffers removeAllObjects];
    for (NSUInteger i = 0; i < _respTabTitles.count; i++) [_respBuffers addObject:@""];
    if (_respBuffers.count) _respBuffers[0] = [NSString stringWithFormat:@"[%@] %@", kind, N(err.message)];
    _activeRespTab = 0;
    _respText.string = _respBuffers[0];
    [self highlightActiveTab:_respTabButtons active:0];
    [self toastWarn:[NSString stringWithFormat:@"%@: %@", kind, N(err.message)]];
}

- (void)finishSending {
    _sending = NO;
    [self stopSendSpinner];
    _sendButton.enabledState = _hasRequest;
    _sendButton.icon = OS9SendImage(16);   // trả lại icon send
    _cancelButton.enabledState = NO;
    [self relayout];
}

// Spinner trong nút Send khi đang gửi: quay 1 nan mỗi tick (8 nan -> ~mượt).
- (void)startSendSpinner {
    _spinPhase = 0;
    [_spinTimer invalidate];
    _sendButton.icon = OS9SpinnerImage(16, 0);
    __weak MainWindowController *ws = self;
    _spinTimer = [NSTimer scheduledTimerWithTimeInterval:0.09 repeats:YES block:^(NSTimer *t) {
        MainWindowController *s = ws; if (!s) { [t invalidate]; return; }
        s->_spinPhase += 1.0 / 8.0;
        if (s->_spinPhase >= 1.0) s->_spinPhase -= 1.0;
        s->_sendButton.icon = OS9SpinnerImage(16, s->_spinPhase);
    }];
}
- (void)stopSendSpinner { [_spinTimer invalidate]; _spinTimer = nil; }

- (void)rebuildResponseBuffers {
    [_respBuffers removeAllObjects];
    using namespace core;
    const ApiResponse &r = _lastResp;
    if (_model.type == RequestType::Http) {
        [_respBuffers addObject:[self applyView:r.body]]; // body theo chế độ Pretty/Raw/Encode/Decode
        [_respBuffers addObject:N(fieldcodec::formatJson(fieldcodec::keyValuesToJson(r.headers), true))];
        [_respBuffers addObject:N(fieldcodec::formatJson(r.resolvedRequestDump, true))];
        NSMutableString *ck = [NSMutableString string];
        for (const auto &c : r.cookies)
            [ck appendFormat:@"%s=%s  (domain=%s path=%s expires=%s)\n", c.name.c_str(), c.value.c_str(),
                             c.domain.c_str(), c.path.c_str(), c.expires.c_str()];
        [_respBuffers addObject:(ck.length ? ck : @"(no Set-Cookie)")];
    } else {
        [_respBuffers addObject:[self applyView:r.body]];
        [_respBuffers addObject:N(fieldcodec::formatJson(r.resolvedRequestDump, true))];
    }
    if (_activeRespTab >= (NSInteger)_respBuffers.count) _activeRespTab = 0;
    _respText.string = _respBuffers.count ? _respBuffers[_activeRespTab] : @"";
    [self highlightActiveTab:_respTabButtons active:_activeRespTab];
}

#pragma mark Status line

- (void)updateStatus:(NSString *)text {
    _statusLabel.stringValue = text ?: @"";
    _statusLabel.textColor = [NSColor blackColor];
}

- (void)updateStatusFromResponse:(const core::ApiResponse &)r error:(BOOL)isErr {
    NSString *code = r.statusCode ? [NSString stringWithFormat:@"%d", r.statusCode] : @"OK";
    NSString *size = (r.sizeBytes >= 1024) ? [NSString stringWithFormat:@"%.1fkb", r.sizeBytes / 1024.0]
                                           : [NSString stringWithFormat:@"%lldb", (long long)r.sizeBytes];
    _statusLabel.stringValue = [NSString stringWithFormat:@"%@ | %ldms | %@", code, r.elapsedMs, size];
    _statusLabel.textColor = (r.statusCode >= 400) ? [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0]
                                                   : [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1.0];
}

#pragma mark ENV

- (void)envClicked:(id)sender {
    if (!_engine) { [self toastWarn:@"Open a collection folder first"]; return; }
    NSMutableArray<NSString *> *items = [@[ @"Global" ] mutableCopy];
    for (const auto &name : _engine->environments().list())
        if (name != "Global") [items addObject:N(name)];
    [items addObject:@"Manage…"];
    NSString *active = N(_engine->session().getActiveEnv());
    NSInteger sel = [items indexOfObject:active]; if (sel == NSNotFound) sel = 0;
    __weak MainWindowController *ws = self;
    OS9ShowDropdown(items, sel, _envButton, ^(NSInteger idx) {
        MainWindowController *s = ws; if (!s) return;
        if (idx == (NSInteger)items.count - 1) { [s manageEnv:nil]; return; }
        [s pickEnvNamed:items[idx]];
    });
}
- (void)pickEnvNamed:(NSString *)name {
    if (!_engine) return;
    _engine->session().setActiveEnv(name.UTF8String);
    [self refreshEnvButton];
    [self toast:[NSString stringWithFormat:@"ENV: %@", name]];
}
- (void)refreshEnvButton {
    _envButton.title = _engine ? N(_engine->session().getActiveEnv()) : @"Global";
}

#pragma mark Config screen (ENV + Setting)

// Setting button -> màn Settings; ENV "Manage…" -> màn Environments (2 màn riêng).
- (void)settingClicked:(id)sender { [self enterConfig:1]; }

- (void)enterConfig:(NSInteger)kind {
    if (!_engine) { [self toastWarn:@"Open a collection folder first"]; return; }
    [self autosaveCurrent];
    _configKind = kind;
    if (kind == 0) {
        _configTitle.stringValue = @"Environments";
        NSView *ev = _envVC.view;
        if (ev.superview != _configPane) [_configPane addSubview:ev];
        ev.hidden = NO;
        _settingScroll.hidden = YES;
        [_envVC reload];
    } else {
        _configTitle.stringValue = @"Settings";
        if (_envVC.view) _envVC.view.hidden = YES;
        _settingScroll.hidden = NO;
        core::AppConfig c = _engine->appConfig().load();
        _settingText.string = [NSString stringWithFormat:
            @"{\n  \"defaultTimeoutMs\": %d,\n  \"verifyTls\": %@,\n  \"fontName\": \"%s\",\n  \"fontSize\": %d\n}",
            c.defaultTimeoutMs, c.verifyTls ? @"true" : @"false", c.fontName.c_str(), c.fontSize];
    }
    _configMode = YES;
    _mainPane.hidden = YES;
    _configPane.hidden = NO;
    [self relayout];
}

- (void)exitConfig:(id)sender {
    // Auto-save khi back, theo đúng màn đang mở.
    if (_engine) {
        if (_configKind == 0) {
            [_envVC save];
        } else {
            NSData *d = [_settingText.string dataUsingEncoding:NSUTF8StringEncoding];
            NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
            if (dict) {
                core::AppConfig c = _engine->appConfig().load();
                if (dict[@"defaultTimeoutMs"]) c.defaultTimeoutMs = [dict[@"defaultTimeoutMs"] intValue];
                if (dict[@"verifyTls"]) c.verifyTls = [dict[@"verifyTls"] boolValue];
                if (dict[@"fontName"]) c.fontName = [dict[@"fontName"] UTF8String];
                if (dict[@"fontSize"]) c.fontSize = [dict[@"fontSize"] intValue];
                _engine->appConfig().save(c);
                [self applyConfiguredFontAndRefresh];
            } else {
                [self toastWarn:@"Invalid settings JSON — skipped"];
            }
        }
    }
    _configMode = NO;
    _configPane.hidden = YES;
    _mainPane.hidden = NO;
    [self refreshEnvButton];
    [self relayout];
    [self toastOk:@"Saved"];
}

#pragma mark Proto source (gRPC)

// Dropdown nguồn proto: index 0 = Reflection, 1 = .proto (mở file panel).
- (void)protoModeChanged:(id)sender {
    if (_model.type != core::RequestType::Grpc) return;
    if (_protoPopup.selectedIndex == 1) {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.allowedFileTypes = @[ @"proto" ];
        if ([p runModal] == NSModalResponseOK) {
            core::ProtoSource ps;
            ps.mode = "protoFiles";
            ps.files.push_back(p.URL.lastPathComponent.UTF8String);
            ps.importPaths.push_back(p.URL.URLByDeletingLastPathComponent.path.UTF8String);
            _model.grpc.protoSource = ps;
        } else {
            // Huỷ chọn file -> trở về trạng thái trước (reflection).
            _protoPopup.selectedIndex = 0;
            [_protoPopup setNeedsDisplay:YES];
            _model.grpc.protoSource = core::ProtoSource{};
            _model.grpc.protoSource.mode = "reflection";
        }
    } else {
        _model.grpc.protoSource = core::ProtoSource{};
        _model.grpc.protoSource.mode = "reflection";
    }
    [self reloadGrpcMethods];
}

#pragma mark RPC picker (gRPC)

// Hiện RPC đã lưu trong model lên nút (KHÔNG gọi mạng). Fetch thật khi bấm vào dropdown.
- (void)showSavedGrpcMethodLabel {
    _grpcMethods.clear();
    const core::GrpcRequest &g = _model.grpc;
    if (g.service.empty() || g.method.empty()) {
        _servicePopup.itemTitles = @[ @"No rpc" ];
        _servicePopup.toolTip = nil;
    } else {
        NSString *full = [NSString stringWithFormat:@"%s/%s", g.service.c_str(), g.method.c_str()];
        _servicePopup.itemTitles = @[ full ];
        _servicePopup.toolTip = full;
    }
    _servicePopup.selectedIndex = 0;
    [_servicePopup setNeedsDisplay:YES];
}

// Nạp nền (KHÔNG bung menu): dùng khi đổi nguồn proto / commit URL.
- (void)reloadGrpcMethods { [self fetchGrpcMethodsThenOpen:NO]; }

// Nạp danh sách service/RPC theo nguồn proto hiện tại (reflection: query host; .proto: parse).
// openWhenDone = YES: bung menu ngay sau khi nạp xong (dùng khi bấm vào dropdown chọn RPC).
// Chạy nền vì reflection có IO mạng; chỉ áp kết quả của lần gọi mới nhất.
- (void)fetchGrpcMethodsThenOpen:(BOOL)openWhenDone {
    if (_model.type != core::RequestType::Grpc || !_engine) return;
    _model.grpc.target = _urlField.stringValue.UTF8String; // target = ô URL (host:port)
    // Reflection cần host; .proto thì parse file nên không bắt buộc host.
    BOOL needsHost = (_model.grpc.protoSource.mode == "reflection");
    if (needsHost && _model.grpc.target.empty()) {
        _grpcMethods.clear();
        _servicePopup.itemTitles = @[ @"No rpc" ];
        _servicePopup.selectedIndex = 0;
        _servicePopup.toolTip = nil;
        [_servicePopup setNeedsDisplay:YES];
        if (openWhenDone) [self toastWarn:@"Nhập host gRPC trước (vd: localhost:50051)"];
        return;
    }
    _servicePopup.itemTitles = @[ @"(loading…)" ];
    _servicePopup.selectedIndex = 0;
    [_servicePopup setNeedsDisplay:YES];

    uint64_t seq = ++_grpcMethodsReqSeq;
    core::GrpcRequest g = _model.grpc;
    core::Engine *engine = _engine.get();
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::string err;
        std::vector<core::GrpcMethodInfo> methods = engine->listGrpcMethods(g, err);
        NSString *errStr = err.empty() ? nil : N(err);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s = ws;
            if (!s || seq != s->_grpcMethodsReqSeq) return; // đã có yêu cầu mới hơn
            [s applyGrpcMethods:methods error:errStr openMenu:openWhenDone];
        });
    });
}

- (void)applyGrpcMethods:(const std::vector<core::GrpcMethodInfo> &)methods error:(NSString *)err
                openMenu:(BOOL)openMenu {
    _grpcMethods = methods;
    if (methods.empty()) {
        _servicePopup.itemTitles = @[ @"No rpc" ];
        _servicePopup.selectedIndex = 0;
        _servicePopup.toolTip = nil;
        [_servicePopup setNeedsDisplay:YES];
        if (err.length) [self toastWarn:[NSString stringWithFormat:@"List RPCs: %@", err]];
        return;
    }
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    NSInteger sel = 0;
    for (size_t i = 0; i < methods.size(); ++i) {
        const core::GrpcMethodInfo &m = methods[i];
        [titles addObject:[NSString stringWithFormat:@"%s/%s", m.service.c_str(), m.method.c_str()]];
        if (m.service == _model.grpc.service && m.method == _model.grpc.method) sel = (NSInteger)i;
    }
    _servicePopup.itemTitles = titles;
    _servicePopup.selectedIndex = sel;
    [_servicePopup setNeedsDisplay:YES];
    [self applySelectedGrpcMethod:sel]; // đồng bộ model với lựa chọn hiển thị
    if (openMenu) [_servicePopup openMenu];
}

// Người dùng chọn RPC -> ghi service/method/methodType vào model (autosave tự lưu).
- (void)serviceMethodChanged:(id)sender {
    [self applySelectedGrpcMethod:_servicePopup.selectedIndex];
}

- (void)applySelectedGrpcMethod:(NSInteger)idx {
    if (idx < 0 || idx >= (NSInteger)_grpcMethods.size()) return;
    const core::GrpcMethodInfo &m = _grpcMethods[(size_t)idx];
    _model.grpc.service = m.service;
    _model.grpc.method = m.method;
    _model.grpc.methodType = m.methodType;
    // Hover nút hiện tên RPC đầy đủ (nút có thể đã cắt "…").
    _servicePopup.toolTip = [NSString stringWithFormat:@"%s/%s", m.service.c_str(), m.method.c_str()];
}

- (void)manageEnv:(id)sender { [self enterConfig:0]; }

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

- (void)deleteSelectedMulti:(id)sender {
    NSMutableArray<TreeItem *> *items = [NSMutableArray array];
    [_tree.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger idx, BOOL *stop) {
        TreeItem *t = [_tree itemAtRow:idx];
        if (t.relPath.length) [items addObject:t];
    }];
    if (items.count == 0) return;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Xoá %lu mục đã chọn?", (unsigned long)items.count];
    [a addButtonWithTitle:@"Delete"]; [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
    [self closeEditorIfDeleted:items];    // tránh autosave tạo lại file vừa xoá
    for (TreeItem *t in items) { try { _engine->collection().remove(t.relPath.UTF8String); } catch (...) {} }
    [self reloadTree];
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
        std::string rel = _engine->collection().createRequest([self selectedFolderRel], t, name.UTF8String);
        [self reloadTree];
        [self loadRequestAtRel:N(rel)];
        [self toastOk:[NSString stringWithFormat:@"Created: %@", name]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)newFolder:(id)s {
    if (!_engine) { [self toastWarn:@"Open a collection folder first"]; return; }
    try { _engine->collection().createFolder([self selectedFolderRel], "New Folder"); [self reloadTree]; }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
// Rename: chỉnh ngay trên cây (inline), không popup.
- (void)renameSel:(id)s {
    [self beginInlineRenameRow:_tree.selectedRow];
}
- (void)dupSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    [self autosaveCurrent];   // flush edits hiện tại trước (tránh trạng thái treo)
    try {
        std::string dupRel = _engine->collection().duplicate(t.relPath.UTF8String);
        [self reloadTree];
        if (!t.isFolder) [self loadRequestAtRel:N(dupRel)];   // mở bản sao -> _currentRel đúng
        [self toastOk:@"Duplicated"];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)deleteSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Xoá %@?", t.name];
    [a addButtonWithTitle:@"Delete"]; [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
    [self closeEditorIfDeleted:@[ t ]];   // tránh autosave tạo lại file vừa xoá
    try { _engine->collection().remove(t.relPath.UTF8String); [self reloadTree]; }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

#pragma mark Window / misc

// performClose: vô hiệu với window borderless -> gọi windowShouldClose: (tự lưu) rồi close trực tiếp.
- (void)closeWindow:(id)sender {
    if ([self windowShouldClose:_window]) [_window close];
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

#pragma mark Toast (retro, stack góc phải-dưới, đẩy lên)

- (void)toast:(NSString *)msg     { [self showToast:msg kind:0]; } // info (xám)
- (void)toastOk:(NSString *)msg   { [self showToast:msg kind:1]; } // success (xanh)
- (void)toastWarn:(NSString *)msg { [self showToast:msg kind:2]; } // fail (đỏ)

- (void)showToast:(NSString *)msg kind:(NSInteger)kind {
    if (!_toasts) _toasts = [NSMutableArray array];
    NSView *cv = _window.contentView;
    OS9Toast *t = [[OS9Toast alloc] initWithMessage:msg kind:kind];
    NSSize sz = [OS9Toast sizeForMessage:msg];
    // bắt đầu off-screen bên phải, ở slot dưới cùng -> reflow sẽ trượt vào.
    t.frame = NSMakeRect(cv.bounds.size.width, cv.bounds.size.height - 14 - sz.height, sz.width, sz.height);
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

// Xếp toast từ góc phải-DƯỚI lên: mới nhất (cuối mảng) ở dưới cùng (content flipped: y lớn = dưới).
- (void)reflowToasts {
    NSView *cv = _window.contentView;
    CGFloat W = cv.bounds.size.width, H = cv.bounds.size.height;
    const CGFloat margin = 14, gap = 8;
    CGFloat bottom = H - margin;
    for (NSInteger i = (NSInteger)_toasts.count - 1; i >= 0; i--) {
        OS9Toast *t = _toasts[i];
        CGFloat tw = t.frame.size.width, th = t.frame.size.height;
        NSRect target = NSMakeRect(W - tw - margin, bottom - th, tw, th);
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.2; t.animator.frame = target; t.animator.alphaValue = 1.0;
        } completionHandler:nil];
        bottom = (bottom - th) - gap;
    }
}

- (void)positionToast { [self reflowToasts]; }

@end
