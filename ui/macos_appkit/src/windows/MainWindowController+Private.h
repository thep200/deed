// MainWindowController+Private.h — class extension dùng CHUNG cho mọi file cài đặt của
// MainWindowController (file chính + các category Tree/Editor/Send/Config/Stress).
//
// Vì sao cần: MainWindowController quá dài nên tách thành nhiều category ở file .mm riêng.
// Category KHÔNG khai báo được ivar; ngược lại ivar khai báo trong @implementation chỉ thấy
// trong CHÍNH file đó. Giải pháp chuẩn (runtime 64-bit macOS): khai báo TẤT CẢ ivar trong
// MỘT class extension đặt ở header này, mọi file cài đặt #import -> đều truy cập được ivar.
//
// Header này cũng gom các import + helper N()/S() dùng chung -> mỗi category chỉ cần
// #import file này (cộng vài header riêng nếu cần).
#pragma once

#import "windows/MainWindowController.h"

#import "app/AppStrings.h"
#import "app/DeedConfig.h"
#import "app/OS9Lifecycle.h"
#import "dialogs/OS9Dialog.h"
#import "windows/EnvWindowController.h"
#import "windows/TreeViews.h"
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
#include <vector>

#include "core/engine.hpp"
#include "core/codec/field_codec.hpp"
#include "core/import_export/importer.hpp"
#include "core/persistence/stores.hpp"
#include "core/persistence/request_naming.hpp"
#include "core/types.hpp"

// Cầu nối std::string <-> NSString dùng khắp nơi.
static inline NSString *N(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }
static inline std::string S(NSString *s) { return s ? std::string(s.UTF8String) : std::string(); }

// Conformance protocol gắn vào ĐÚNG category cài đặt -> không có cảnh báo
// -Wprotocol / -Wobjc-protocol-method-implementation, mà mọi call site #import header này
// đều thấy self conform (vd dựng UiDelegateBridge cần <CoreResponseSink>).
@interface MainWindowController (Tree) <NSOutlineViewDataSource, NSOutlineViewDelegate>
@end
@interface MainWindowController (Editor) <NSTextFieldDelegate, NSTextViewDelegate>
@end
@interface MainWindowController (Send) <CoreResponseSink>
@end

@interface MainWindowController () {
@protected
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
    NSMutableArray<TreeItem *> *_roots;
    NSMutableSet<NSString *> *_expandedFolders; // relPath các folder đang mở (giữ qua reload)
    BOOL _revealingSelection;                    // đang chọn do reveal -> bỏ qua auto-load

    // (3) request editor
    OS9SerratedInset *_reqInset;
    SciTextView *_reqText;   // editor request (Scintilla)
    NSMutableArray<NSString *> *_reqBuffers;
    NSMutableArray<OS9BevelButton *> *_reqTabButtons;
    NSArray<NSString *> *_reqTabTitles;
    NSInteger _activeReqTab;
    // VIỆC 1: nhớ tab theo TỪNG pane bằng KHOÁ ngữ nghĩa (title), KHÔNG theo index, KHÔNG
    // nằm trong RequestModel -> giữ qua chuyển request. nil/không-khớp -> fallback tab đầu.
    NSString *_leftPaneActiveTabKey;

    // (4) response
    OS9SerratedInset *_respInset;
    SciTextView *_respText;  // editor response (Scintilla, read-only)
    NSMutableArray<NSString *> *_respBuffers;
    NSMutableArray<OS9BevelButton *> *_respTabButtons;
    NSArray<NSString *> *_respTabTitles;
    NSInteger _activeRespTab;
    NSString *_rightPaneActiveTabKey;   // khoá tab pane phải (nhớ riêng pane này)
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

    // config screen (2 màn riêng: Environments / Settings) — tiêu đề hiển thị ở title bar
    OS9BevelButton *_backButton;
    NSInteger _configKind; // 0 = Environments, 1 = Settings
    EnvWindowController *_envVC;
    OS9SerratedInset *_settingInset;   // viền răng cưa bao editor Settings (giống các pane khác)
    SciTextView *_settingEditor;   // editor JSON cho Settings (Scintilla — lexer JSON + theme Platinum)

    BOOL _sending;
    NSTimer *_spinTimer;   // animate icon loading trong nút Send
    CGFloat _spinPhase;
    NSUInteger _urlPrevLen; // độ dài URL lần trước -> nhận biết "dán" (tăng đột biến)
    NSTextView *_fieldEditor; // field editor dùng chung, tắt hết auto-features
    NSString *_bodyMode;    // mode body hiện tại: json | binary(file) | form-urlencoded(form)
}
@end

// Khai báo MỌI method nội bộ để các category gọi chéo file được (ARC cần thấy selector).
// Sinh tự động từ các định nghĩa trong MainWindowController*.mm.
@interface MainWindowController (Internal)
- (void)showWindow;
- (void)restoreLastCollection;
- (void)buildChrome;
- (void)buildToast;
- (void)disableAutoFeatures:(NSTextView *)tv;
- (id)windowWillReturnFieldEditor:(NSWindow *)sender toObject:(id)client;
- (void)styleScroller:(NSScrollView *)sc;
- (void)buildTree;
- (void)buildEditors;
- (NSString *)prettyTitle;
- (NSString *)applyView:(const std::string &)body;
- (NSString *)applyView:(const std::string &)body mode:(int)mode;   // U2: không đọc ivar -> chạy nền được
- (void)buildStatusBar;
- (void)buildToolbar;
- (void)buildDividers;
- (void)buildConfigPane;
- (void)relayout;
- (void)layoutTabButtons:(NSArray<OS9BevelButton *> *)buttons atX:(CGFloat)x y:(CGFloat)y width:(CGFloat)width height:(CGFloat)h extra:(CGFloat)extra;
- (void)layoutConfig;
- (void)setRequestType:(core::RequestType)t;
- (void)rebuildTabButtons;
- (void)setHasRequest:(BOOL)has;
- (void)openFolder:(id)sender;
- (core::AppConfig)appDefaultsFromEnv;
- (void)openCollectionRoot:(NSString *)path;
- (BOOL)resyncCurrentRelById;
- (void)loadChildrenOf:(TreeItem *)folder;
- (void)reloadTree;
- (void)restoreExpansion:(NSArray<TreeItem *> *)items;
// Cập nhật tăng dần (§T1): chỉ quét lại cấp `parentRel` ("" = gốc), giữ nguyên các TreeItem
// (và con đã nạp) của nhánh khác -> không re-scan toàn bộ folder đang mở.
- (void)refreshTreeLevel:(NSString *)parentRel;
- (void)reselectTreeByRel:(NSString *)rel;   // chọn lại node theo relPath sau cập nhật (không auto-load)
- (TreeItem *)loadedFolderItemForRel:(NSString *)rel;
- (void)mergeScanLevel:(const std::string &)rel into:(NSMutableArray<TreeItem *> *)items;
// §T3: đổi prefix relPath trong _expandedFolders khi rename/move folder để giữ trạng thái mở.
- (void)remapExpandedFoldersFrom:(NSString *)oldRel to:(NSString *)newRel;
- (void)treeClicked:(id)sender;
- (void)treeDoubleClicked:(id)sender;
- (void)promptRenameItem:(TreeItem *)t;
- (void)outlineViewItemDidExpand:(NSNotification *)n;
- (void)outlineViewItemDidCollapse:(NSNotification *)n;
- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item;
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)idx ofItem:(id)item;
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item;
- (NSTableRowView *)outlineView:(NSOutlineView *)ov rowViewForItem:(id)item;
- (NSView *)outlineView:(NSOutlineView *)ov viewForTableColumn:(NSTableColumn *)col item:(id)item;
- (void)outlineViewSelectionDidChange:(NSNotification *)note;
- (void)revealAndSelectRequestById:(NSString *)reqId relPath:(NSString *)relPath;
- (id<NSPasteboardWriting>)outlineView:(NSOutlineView *)ov pasteboardWriterForItem:(id)item;
- (NSDragOperation)outlineView:(NSOutlineView *)ov validateDrop:(id<NSDraggingInfo>)info                    proposedItem:(id)item proposedChildIndex:(NSInteger)idx;
- (BOOL)outlineView:(NSOutlineView *)ov acceptDrop:(id<NSDraggingInfo>)info item:(id)item childIndex:(NSInteger)idx;
- (void)cancelInFlightForSwitch;
- (void)loadRequestAtRel:(NSString *)rel;
- (void)showCachedResponseForId:(const std::string &)reqId;
- (void)populateEditorsFromModel;
- (NSInteger)tabIndexForKey:(NSString *)key inTitles:(NSArray<NSString *> *)titles;
- (void)stashActiveReqBuffer;
- (BOOL)syncModelFromEditors:(BOOL)silent;
- (void)closeEditorIfDeleted:(NSArray<TreeItem *> *)deleted;
- (void)autosaveCurrent;
- (NSString *)bodyButtonTitle;
- (NSInteger)bodyTabIndex;
- (void)updateBodyButtonLabel;
- (NSString *)bodyTemplateForMode:(NSString *)mode;
- (NSString *)bodyBufferFromModel:(const core::Body &)b;
- (BOOL)syncBodyFromBuffer:(NSString *)buf into:(core::Body &)out err:(std::string &)err;
- (void)bodyButtonClicked:(OS9BevelButton *)b;
- (void)pickBodyMode:(NSString *)mode;
- (void)reqTabClicked:(OS9BevelButton *)b;
- (void)selectReqTab:(NSInteger)tab;
- (void)respTabClicked:(OS9BevelButton *)b;
- (void)highlightActiveTab:(NSArray<OS9BevelButton *> *)buttons active:(NSInteger)active;
- (void)prettyToggle:(id)sender;
- (void)applyPrettyToFocusedPane;
- (void)copyAsCurl:(id)sender;
- (void)zoomToggle:(id)sender;
- (void)collapseToggle:(id)sender;
- (void)applyConfiguredFontAndRefresh;
- (void)controlTextDidChange:(NSNotification *)note;
- (void)importNow:(NSString *)text grpc:(BOOL)isGrpc;
- (void)offerImport:(NSString *)text grpc:(BOOL)isGrpc;
- (NSString *)importSummary:(const core::RequestModel &)m unknown:(const std::vector<std::string> &)unknown grpc:(BOOL)isGrpc;
- (NSString *)deriveImportName:(const core::RequestModel &)m;
- (void)applyImport:(const core::RequestModel &)m;
- (void)restoreUrlField;
- (void)methodChanged:(id)sender;
- (void)urlCommitted:(id)sender;
- (NSString *)urlDecodeComponent:(NSString *)s;
- (void)parseUrlQueryIntoQueryTab;
- (void)updateTitle;
- (void)saveRequest:(id)sender;
- (void)sendRequest:(id)sender;
- (void)cancelClicked:(id)sender;
- (void)onCoreResponse:(uint64_t)handle response:(const core::ApiResponse &)resp;
- (void)onCoreError:(uint64_t)handle error:(const core::ApiError &)err;
- (void)displayErrorKind:(core::ErrorKind)kind message:(NSString *)msg;
- (void)cacheResponseAsync:(const core::ApiResponse &)resp forId:(const std::string &)reqId;
- (void)cacheErrorAsync:(const core::ApiError &)err forId:(const std::string &)reqId;
- (void)finishSending;
- (void)startSendSpinner;
- (void)stopSendSpinner;
- (void)rebuildResponseBuffers;
- (void)rebuildResponseBuffersAsync;   // U2: format response ngoài main thread
- (NSArray<NSString *> *)computeResponseBuffersFor:(const core::ApiResponse &)r
                                              type:(core::RequestType)type
                                        prettyMode:(int)prettyMode;
- (void)applyResponseBuffers:(NSArray<NSString *> *)bufs;
- (void)updateStatus:(NSString *)text;
- (NSString *)clockFromEpochMs:(int64_t)ms;
- (void)updateStatusFromResponse:(const core::ApiResponse &)r error:(BOOL)isErr endMs:(int64_t)endMs;
- (void)envClicked:(id)sender;
- (void)pickEnvNamed:(NSString *)name;
- (void)refreshEnvButton;
- (void)settingClicked:(id)sender;
- (void)enterConfig:(NSInteger)kind;
- (void)exitConfig:(id)sender;
- (void)protoModeChanged:(id)sender;
- (void)showSavedGrpcMethodLabel;
- (void)reloadGrpcMethods;
- (void)fetchGrpcMethodsThenOpen:(BOOL)openWhenDone;
- (void)applyGrpcMethods:(const std::vector<core::GrpcMethodInfo> &)methods error:(NSString *)err                 openMenu:(BOOL)openMenu;
- (void)serviceMethodChanged:(id)sender;
- (void)applySelectedGrpcMethod:(NSInteger)idx;
- (void)manageEnv:(id)sender;
- (NSMenu *)contextMenuForRow:(NSInteger)row;
- (void)purgeCacheAtRel:(NSString *)rel isFolder:(BOOL)isFolder;
- (void)deleteSelectedMulti:(id)sender;
- (std::string)selectedFolderRel;
- (void)newHttp:(id)s;
- (void)newGrpc:(id)s;
- (void)createRequest:(core::RequestType)t name:(NSString *)name;
- (void)newFolder:(id)s;
- (void)renameSel:(id)s;
- (void)dupSel:(id)s;
- (void)deleteSel:(id)s;
- (void)closeWindow:(id)sender;
- (BOOL)windowShouldClose:(NSWindow *)sender;
- (void)windowDidResize:(NSNotification *)note;
- (void)windowDidBecomeKey:(NSNotification *)note;
- (void)windowDidResignKey:(NSNotification *)note;
- (NSString *)abbreviatePath:(NSString *)path;
- (void)toast:(NSString *)msg;
- (void)toastOk:(NSString *)msg;
- (void)toastWarn:(NSString *)msg;
- (void)showToast:(NSString *)msg kind:(NSInteger)kind;
- (void)dismissToast:(OS9Toast *)t;
- (void)reflowToasts;
- (void)positionToast;
@end
