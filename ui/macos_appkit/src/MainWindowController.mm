#import "MainWindowController.h"

#import "DeedConfig.h"
#import "EnvWindowController.h"
#import "OS9Theme.h"
#import "OS9Widgets.h"
#import "SciTextView.h"

#include <memory>
#include <string>

#include "core/engine.hpp"
#include "core/field_codec.hpp"
#include "core/importer.hpp"
#include "core/stores.hpp"
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
    OS9BevelButton *_protoButton;
    OS9PopupButton *_methodPopup;
    OS9SerratedInset *_urlInset; // khung input răng cưa retro bọc ô URL
    NSTextField *_urlField;

    // dividers + bề rộng pane
    OS9Divider *_divTree;
    OS9Divider *_divResp;
    CGFloat _treeW;
    CGFloat _reqW;
    NSRect _preZoomFrame; // lưu frame trước khi phóng to (để thu nhỏ lại)

    // toast
    NSTextField *_toast;
    NSUInteger _toastGen;

    // config screen (2 màn riêng: Environments / Settings)
    OS9BevelButton *_backButton;
    NSTextField *_configTitle;
    NSInteger _configKind; // 0 = Environments, 1 = Settings
    EnvWindowController *_envVC;
    NSScrollView *_settingScroll;
    NSTextView *_settingText;

    BOOL _sending;
}

#pragma mark Build

- (void)showWindow {
    DeedConfig *cfg = [DeedConfig shared];
    // Font hiển thị lấy từ Settings (app-support) — set TRƯỚC khi dựng widget.
    { core::AppConfigStore a; core::AppConfig c = a.load();
      [OS9Theme setConfiguredFontName:N(c.fontName) size:c.fontSize]; }
    // Kiểu nút: new (btn-new.svg) mặc định, hoặc classic (button.svg) qua .env.
    [OS9Theme setClassicButtonStyle:[[cfg stringFor:@"BUTTON_STYLE" def:@"new"] isEqualToString:@"classic"]];
    NSRect frame = NSMakeRect(0, 0, [cfg floatFor:@"WINDOW_WIDTH" def:1040], [cfg floatFor:@"WINDOW_HEIGHT" def:680]);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                     NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                                                     NSWindowStyleMaskFullSizeContentView)
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.titlebarAppearsTransparent = YES;
    _window.titleVisibility = NSWindowTitleHidden;
    _window.movableByWindowBackground = NO;
    [_window standardWindowButton:NSWindowCloseButton].hidden = YES;
    [_window standardWindowButton:NSWindowMiniaturizeButton].hidden = YES;
    [_window standardWindowButton:NSWindowZoomButton].hidden = YES;
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
    [_window.contentView addSubview:_titleBar];
}

- (void)buildToast {
    _toast = [NSTextField labelWithString:@""];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.font = [OS9Theme uiFont];
    _toast.textColor = [NSColor whiteColor];
    _toast.wantsLayer = YES;
    _toast.layer.backgroundColor = [NSColor colorWithCalibratedWhite:0.12 alpha:0.92].CGColor;
    _toast.layer.cornerRadius = 4;
    _toast.drawsBackground = NO;
    _toast.bezeled = NO;
    _toast.editable = NO;
    _toast.hidden = YES;
    [_window.contentView addSubview:_toast positioned:NSWindowAbove relativeTo:nil];
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
    _tree.action = @selector(treeClicked:); // click folder -> fold/unfold
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
    __weak MainWindowController *weakSelf = self;
    // (3) request: editor Scintilla sửa được, live-stash khi user gõ.
    _reqInset = [[OS9SerratedInset alloc] initWithFrame:NSZeroRect];
    [_mainPane addSubview:_reqInset];
    _reqText = [[SciTextView alloc] initEditable:YES];
    _reqText.onTextChanged = ^{ [weakSelf reqTextChanged]; };
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
- (NSString *)prettyTitle { return @[ @"{ } Pretty", @"{} Raw", @"\" \" Encode", @"\" \" Decode" ][_prettyMode]; }
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
    _statusLabel = OS9Label(@"");
    _statusLabel.alignment = NSTextAlignmentCenter;   // căn giữa text status
    [_mainPane addSubview:_statusLabel];
}

- (void)buildToolbar {
    _settingButton = [[OS9BevelButton alloc] initWithTitle:@"" target:self action:@selector(settingClicked:)];
    _settingButton.icon = OS9GearImage(16);   // bánh răng cổ điển thay cho chữ "Setting"
    _settingButton.toolTip = @"Settings";
    _envButton = [[OS9BevelButton alloc] initWithTitle:@"Global" target:self action:@selector(envClicked:)];
    _envButton.dropdown = YES;   // hiển thị mũi tên dropdown như method
    _sendButton = [[OS9BevelButton alloc] initWithTitle:@"Send  ⌘↩" target:self action:@selector(sendRequest:)];
    _sendButton.isDefault = YES;
    _cancelButton = [[OS9BevelButton alloc] initWithTitle:@"Cancel" target:self action:@selector(cancelClicked:)];
    _protoButton = [[OS9BevelButton alloc] initWithTitle:@"Proto: reflection" target:self action:@selector(protoClicked:)];

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
    [_urlInset addSubview:_urlField];

    for (NSView *v in @[ _settingButton, _envButton, _sendButton, _cancelButton, _protoButton, _methodPopup, _urlInset ])
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
    _settingText.automaticQuoteSubstitutionEnabled = NO;
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

    // (3) request tabs + [cURL] ở mép phải hàng tab + editor
    CGFloat curlW = 46;
    [self layoutTabButtons:_reqTabButtons atX:reqX y:top width:(_reqW - curlW - 4) height:tabH extra:0];
    _curlButton.frame = NSMakeRect(reqX + _reqW - curlW, top, curlW, tabH);
    _reqInset.frame = NSMakeRect(reqX, panesY, _reqW, panesBottom - panesY);
    _reqText.frame = NSInsetRect(_reqInset.bounds, 2, 2);

    // (4) response tabs (+ pretty) + editor
    CGFloat prettyW = 70;
    [self layoutTabButtons:_respTabButtons atX:respX y:top width:(respW - prettyW - 4) height:tabH extra:0];
    _prettyButton.frame = NSMakeRect(respX + respW - prettyW, top, prettyW, tabH);
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
    CGFloat wProto = [cfg floatFor:@"BTN_PROTO_W" def:150];
    CGFloat wSend = [cfg floatFor:@"BTN_SEND_W" def:96];
    CGFloat wCancel = [cfg floatFor:@"BTN_CANCEL_W" def:64];

    _settingButton.frame = NSMakeRect(x, ty, wSetting, btnH); x += wSetting + 6;
    _envButton.frame = NSMakeRect(x, ty, wEnv, btnH); x += wEnv + 6;
    BOOL grpc = (_model.type == core::RequestType::Grpc);
    _methodPopup.frame = NSMakeRect(x, ty, wMethod, btnH);
    _protoButton.frame = NSMakeRect(x, ty, wProto, btnH);
    _methodPopup.hidden = grpc;
    _protoButton.hidden = !grpc;
    x += (grpc ? wProto : wMethod) + 6;

    _cancelButton.hidden = !_sending;
    CGFloat rightGroup = wSend + 6 + (_sending ? wCancel + 6 : 0);
    CGFloat urlW = (MW - pad) - x - rightGroup;
    if (urlW < 140) urlW = 140;
    _urlInset.frame = NSMakeRect(x, ty, urlW, btnH);
    // field nằm trong inset, chừa viền + canh giữa theo chiều dọc cho 1 dòng.
    CGFloat fh = ceil([[OS9Theme monoFont] ascender] - [[OS9Theme monoFont] descender]) + 2;
    _urlField.frame = NSMakeRect(4, floor((btnH - fh) / 2), urlW - 8, fh);
    if (_sending) _cancelButton.frame = NSMakeRect(MW - pad - wSend - 6 - wCancel, ty, wCancel, btnH);
    _sendButton.frame = NSMakeRect(MW - pad - wSend, ty, wSend, btnH);

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
        _reqTabTitles = @[ @"Params", @"Headers", @"Body", @"Auth" ];
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
        OS9BevelButton *b = [[OS9BevelButton alloc] initWithTitle:t target:self action:@selector(reqTabClicked:)];
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
        _reqText.string = @""; _respText.string = @""; _urlField.stringValue = @"";
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
            else [self toast:[NSString stringWithFormat:@"Không tìm thấy %s, đã bỏ qua", last.c_str()]];
        }
    } catch (...) {}
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
        const HttpRequest &h = _model.http;
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(h.params))];
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(h.headers))];
        [_reqBuffers addObject:N(fieldcodec::bodyToJson(h.body))];
        [_reqBuffers addObject:N(fieldcodec::authToJson(h.auth))];
        [_methodPopup selectTitle:N(h.method)];
        _urlField.stringValue = N(h.url);
    } else {
        const GrpcRequest &g = _model.grpc;
        [_reqBuffers addObject:N(g.message.empty() ? "{}" : g.message)];
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(g.metadata))];
        Auth dummy;
        [_reqBuffers addObject:N(fieldcodec::authToJson(dummy))];
        _urlField.stringValue = N(g.target);
        _protoButton.title = [NSString stringWithFormat:@"Proto: %s", g.protoSource.mode.c_str()];
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
            [self toastWarn:[NSString stringWithFormat:@"JSON sai ở tab %@: %s", tn, e.c_str()]];
        }
        return NO;
    };
    if (_model.type == RequestType::Http) {
        HttpRequest &h = _model.http;
        h.method = _methodPopup.selectedTitle.UTF8String ?: "GET";
        h.url = _urlField.stringValue.UTF8String;
        if (!fieldcodec::jsonToKeyValues(S(_reqBuffers[0]), h.params, err)) return fail(0, err);
        if (!fieldcodec::jsonToKeyValues(S(_reqBuffers[1]), h.headers, err)) return fail(1, err);
        if (!fieldcodec::jsonToBody(S(_reqBuffers[2]), h.body, err)) return fail(2, err);
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
    if (![self syncModelFromEditors:YES]) { [self toastWarn:@"Không tự lưu được: JSON chưa hợp lệ"]; return; }
    try { _engine->collection().saveRequest(_currentRel, _model); [_tree reloadData];
          [self applyTreeExpansion]; }
    catch (...) {}
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
    [self toast:@"Đã copy cURL"];
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

// Live-stash: user gõ trong editor request -> buffer tab hiện tại luôn đồng bộ
// (SciTextView.onTextChanged gọi cái này; tránh lẫn giá trị khi đổi tab).
- (void)reqTextChanged {
    if (_activeReqTab >= 0 && _activeReqTab < (NSInteger)_reqBuffers.count)
        _reqBuffers[_activeReqTab] = [(_reqText.string ?: @"") copy];
}
- (void)controlTextDidChange:(NSNotification *)note { }
- (void)methodChanged:(id)sender { }
- (void)urlCommitted:(id)sender { }

- (void)updateTitle {
    // Title CHỈ là tên request (rỗng nếu chưa chọn). Không còn dấu dirty.
    _titleBar.title = _hasRequest ? N(_model.name) : @"";
    [_titleBar setNeedsDisplay:YES];
}

#pragma mark Save (thủ công vẫn giữ ⌘S)

- (void)saveRequest:(id)sender {
    if (!_hasRequest || !_engine) return;
    if (![self syncModelFromEditors:NO]) return;
    try {
        _engine->collection().saveRequest(_currentRel, _model);
        [self reloadTree];
        [self toast:@"Đã lưu"];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

#pragma mark Send / Cancel

- (void)sendRequest:(id)sender {
    if (!_hasRequest || !_engine || _sending) return;
    if (![self syncModelFromEditors:NO]) return;
    if (_model.type == core::RequestType::Grpc && _model.grpc.methodType != "unary") {
        [self toastWarn:@"POC chỉ gửi gRPC unary"]; return;
    }
    _sending = YES;
    _sendButton.enabledState = NO;
    _sendButton.title = @"Sending…";   // trạng thái nằm ngay trên nút Send
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
    _statusLabel.stringValue = (err.kind == core::ErrorKind::Cancelled) ? @"Đã huỷ" : [NSString stringWithFormat:@"✕ %@", kind];
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
    _sendButton.enabledState = _hasRequest;
    _sendButton.title = @"Send  ⌘↩";
    _cancelButton.enabledState = NO;
    [self relayout];
}

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
    if (!_engine) { [self toastWarn:@"Mở folder collection trước"]; return; }
    NSMenu *m = [[NSMenu alloc] init];
    NSMenuItem *g = [m addItemWithTitle:@"Global" action:@selector(pickEnv:) keyEquivalent:@""]; g.target = self;
    for (const auto &name : _engine->environments().list()) {
        if (name == "Global") continue;
        NSMenuItem *it = [m addItemWithTitle:N(name) action:@selector(pickEnv:) keyEquivalent:@""]; it.target = self;
    }
    [m addItem:[NSMenuItem separatorItem]];
    [[m addItemWithTitle:@"Manage…" action:@selector(manageEnv:) keyEquivalent:@""] setTarget:self];
    OS9StyleMenu(m);
    [m popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, _envButton.frame.size.height) inView:_envButton];
}
- (void)pickEnv:(NSMenuItem *)item {
    if (!_engine) return;
    _engine->session().setActiveEnv(item.title.UTF8String);
    [self refreshEnvButton];
    [self toast:[NSString stringWithFormat:@"ENV: %@", item.title]];
}
- (void)refreshEnvButton {
    _envButton.title = _engine ? N(_engine->session().getActiveEnv()) : @"Global";
}

#pragma mark Config screen (ENV + Setting)

// Setting button -> màn Settings; ENV "Manage…" -> màn Environments (2 màn riêng).
- (void)settingClicked:(id)sender { [self enterConfig:1]; }

- (void)enterConfig:(NSInteger)kind {
    if (!_engine) { [self toastWarn:@"Mở folder collection trước"]; return; }
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
                [self toastWarn:@"Setting JSON sai — bỏ qua"];
            }
        }
    }
    _configMode = NO;
    _configPane.hidden = YES;
    _mainPane.hidden = NO;
    [self refreshEnvButton];
    [self relayout];
    [self toast:@"Đã lưu"];
}

#pragma mark Proto (gRPC)

- (void)protoClicked:(id)sender {
    if (_model.type != core::RequestType::Grpc) return;
    NSMenu *m = [[NSMenu alloc] init];
    [[m addItemWithTitle:@"Reflection (mặc định)" action:@selector(protoReflection:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Chọn .proto…" action:@selector(protoFiles:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Chọn descriptorSet…" action:@selector(protoDescSet:) keyEquivalent:@""] setTarget:self];
    OS9StyleMenu(m);
    [m popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, _protoButton.frame.size.height) inView:_protoButton];
}
- (void)protoReflection:(id)s {
    _model.grpc.protoSource = core::ProtoSource{}; _model.grpc.protoSource.mode = "reflection";
    _protoButton.title = @"Proto: reflection";
}
- (void)protoFiles:(id)s {
    NSOpenPanel *p = [NSOpenPanel openPanel]; p.allowedFileTypes = @[ @"proto" ];
    if ([p runModal] == NSModalResponseOK) {
        core::ProtoSource ps; ps.mode = "protoFiles";
        ps.files.push_back(p.URL.lastPathComponent.UTF8String);
        ps.importPaths.push_back(p.URL.URLByDeletingLastPathComponent.path.UTF8String);
        _model.grpc.protoSource = ps; _protoButton.title = @"Proto: protoFiles";
    }
}
- (void)protoDescSet:(id)s {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    if ([p runModal] == NSModalResponseOK) {
        core::ProtoSource ps; ps.mode = "descriptorSet"; ps.descriptorSetPath = p.URL.path.UTF8String;
        _model.grpc.protoSource = ps; _protoButton.title = @"Proto: descriptorSet";
    }
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
- (NSString *)promptName:(NSString *)title default:(NSString *)def {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = title;
    NSTextField *tf = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)];
    tf.stringValue = def ?: @"";
    a.accessoryView = tf;
    [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"];
    return ([a runModal] == NSAlertFirstButtonReturn) ? tf.stringValue : nil;
}
- (void)newHttp:(id)s { [self createRequest:core::RequestType::Http]; }
- (void)newGrpc:(id)s { [self createRequest:core::RequestType::Grpc]; }
- (void)createRequest:(core::RequestType)t {
    NSString *name = [self promptName:@"Tên request" default:@"New Request"];
    if (!name) return;
    try {
        std::string rel = _engine->collection().createRequest([self selectedFolderRel], t, name.UTF8String);
        [self reloadTree];
        [self loadRequestAtRel:N(rel)];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)newFolder:(id)s {
    NSString *name = [self promptName:@"Tên folder" default:@"New Folder"];
    if (!name) return;
    try { _engine->collection().createFolder([self selectedFolderRel], name.UTF8String); [self reloadTree]; }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)renameSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    NSString *name = [self promptName:@"Tên mới" default:t.name];
    if (!name) return;
    try { _engine->collection().rename(t.relPath.UTF8String, name.UTF8String); [self reloadTree]; }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}
- (void)dupSel:(id)s {
    NSInteger row = _tree.selectedRow; if (row < 0) return;
    TreeItem *t = [_tree itemAtRow:row];
    try { _engine->collection().duplicate(t.relPath.UTF8String); [self reloadTree]; }
    catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
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

- (void)closeWindow:(id)sender { [_window performClose:nil]; }
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

#pragma mark Toast (bay ra từ phải)

- (void)toast:(NSString *)msg { [self showToast:msg warn:NO]; }
- (void)toastWarn:(NSString *)msg { [self showToast:msg warn:YES]; }

- (void)showToast:(NSString *)msg warn:(BOOL)warn {
    if (!_toast) return;
    _toast.stringValue = msg;
    _toast.layer.backgroundColor = (warn ? [NSColor colorWithCalibratedRed:0.45 green:0.10 blue:0.10 alpha:0.95]
                                          : [NSColor colorWithCalibratedWhite:0.12 alpha:0.94]).CGColor;
    [_window.contentView addSubview:_toast positioned:NSWindowAbove relativeTo:nil];
    _toast.hidden = NO;
    _toast.alphaValue = 1.0;
    [self positionToast];

    // trượt vào từ phải
    NSRect on = _toast.frame;
    NSRect off = on; off.origin.x = _window.contentView.bounds.size.width;
    _toast.frame = off;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = 0.22; self->_toast.animator.frame = on;
    } completionHandler:nil];

    NSUInteger gen = ++_toastGen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.6 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (gen != self->_toastGen) return;
        NSRect cur = self->_toast.frame; NSRect away = cur; away.origin.x = self->_window.contentView.bounds.size.width;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.3; self->_toast.animator.frame = away; self->_toast.animator.alphaValue = 0.0;
        } completionHandler:^{ if (gen == self->_toastGen) self->_toast.hidden = YES; }];
    });
}

- (void)positionToast {
    NSRect b = [_window.contentView bounds];
    NSSize sz = [_toast.stringValue sizeWithAttributes:@{NSFontAttributeName : [OS9Theme uiFont]}];
    CGFloat w = MIN(sz.width + 28, b.size.width - 40);
    CGFloat h = 26;
    // góc phải, ngay dưới title bar (content flipped: y nhỏ = trên).
    _toast.frame = NSMakeRect(b.size.width - w - 12, 30, w, h);
}

@end
