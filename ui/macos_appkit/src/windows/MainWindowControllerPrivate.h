// MainWindowControllerPrivate.h — class extension SHARED by every file implementing
// MainWindowController (the main file + the Tree/Editor/Send/Config/Stress categories).
//
// Why needed: MainWindowController is too long, so it's split into several categories in
// separate .mm files. Categories CANNOT declare ivars; conversely ivars declared in an
// @implementation are visible only in THAT file. Standard fix (64-bit macOS runtime):
// declare ALL ivars in ONE class extension in this header; every implementing file #imports it
// and gets access to the ivars.
//
// This header also gathers the shared imports + N()/S() helpers -> each category just needs to
// #import this file (plus a few of its own headers if needed).
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

// std::string <-> NSString bridge used everywhere.
static inline NSString *N(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }
static inline std::string S(NSString *s) { return s ? std::string(s.UTF8String) : std::string(); }

// Conformance protocols attached to the category that implements them -> no
// -Wprotocol / -Wobjc-protocol-method-implementation warnings, while every call site that
// #imports this header sees self conform (e.g. building UiDelegateBridge needs <CoreResponseSink>).
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
    std::string _currentId;   // id of open request (stable identifier)
    uint64_t _currentHandle;
    BOOL _hasRequest;

    // streaming (SPEC_grpc_streaming §7/§10)
    BOOL _streaming;                       // a server-stream is in flight (Stop replaces Send)
    core::StreamHandle _streamHandle;      // handle to cancel the active stream
    core::SessionHandle _wsSession;        // active WebSocket session (SPEC_websocket §4); channel = send side
    NSMutableString *_streamAccum;         // assembled "[ … ]" array, cached on close
    uint64_t _streamEvents;                // events received so far (status line)
    std::vector<core::GrpcMethodInfo> _grpcMethods; // parallel to _servicePopup items (ONLY the current request's)
    uint64_t _grpcMethodsReqSeq;  // race guard: apply only the latest listGrpcMethods result
    BOOL _grpcMethodsFetched;     // true once fetched for THIS request -> reuse, don't re-fetch (until invalidated)
    uint64_t _loadReqSeq;         // token: apply only the LATEST loadRequestAtRel model (async load)

    // Chrome + containers
    NSWindow *_window;
    OS9TitleBar *_titleBar;
    NSView *_mainPane;     // main screen
    NSView *_configPane;   // config screen (ENV + Setting)
    BOOL _configMode;

    // (1)(2) tree
    OS9BevelButton *_openButton;
    OS9SerratedInset *_treeInset;
    NSScrollView *_treeScroll;
    DeedOutlineView *_tree;
    NSMutableArray<TreeItem *> *_roots;
    NSMutableSet<NSString *> *_expandedFolders; // relPaths of expanded folders (kept across reload)
    BOOL _revealingSelection;                    // selecting due to reveal -> skip auto-load

    // (3) request editor
    OS9SerratedInset *_reqInset;
    SciTextView *_reqText;   // request editor (Scintilla)
    NSMutableArray<NSString *> *_reqBuffers;
    NSMutableArray<OS9BevelButton *> *_reqTabButtons;
    NSArray<NSString *> *_reqTabTitles;
    NSInteger _activeReqTab;
    // TASK 1: remember active tab PER pane via a semantic KEY (title), NOT by index, NOT
    // stored in RequestModel -> persists across request switches. nil/no-match -> fall back to first tab.
    NSString *_leftPaneActiveTabKey;

    // (4) response
    OS9SerratedInset *_respInset;
    SciTextView *_respText;  // response editor (Scintilla, read-only)
    NSMutableArray<NSString *> *_respBuffers;
    NSMutableArray<OS9BevelButton *> *_respTabButtons;
    NSArray<NSString *> *_respTabTitles;
    NSInteger _activeRespTab;
    NSString *_rightPaneActiveTabKey;   // active tab key for right pane (remembered separately)
    OS9BevelButton *_prettyButton;
    NSInteger _prettyMode; // 0=Pretty 1=Raw 2=Encode 3=Decode
    OS9BevelButton *_curlButton;      // copy current request as cURL
    core::ApiResponse _lastResp;
    BOOL _hasResp;

    // status line (above panes, below tab buttons)
    OS9SerratedInset *_statusBar;
    NSTextField *_statusLabel;

    // toolbar
    OS9BevelButton *_settingButton;
    OS9BevelButton *_envButton;
    OS9BevelButton *_sendButton;
    OS9BevelButton *_cancelButton;
    OS9PopupButton *_protoPopup;     // gRPC: pick proto source (Reflection | .proto)
    OS9PopupButton *_servicePopup;   // gRPC: pick service/RPC (before the Send button)
    OS9PopupButton *_methodPopup;
    OS9SerratedInset *_urlInset; // retro serrated input frame wrapping the URL field
    NSTextField *_urlField;

    // dividers + pane widths
    OS9Divider *_divTree;
    OS9Divider *_divResp;
    CGFloat _treeW;
    CGFloat _reqW;
    NSRect _preZoomFrame; // frame saved before zooming (to restore on un-zoom)

    // toast (stack at bottom-right, pushed upward)
    NSMutableArray<OS9Toast *> *_toasts;

    // config screen (2 separate screens: Environments / Settings) — title shown in the title bar
    OS9BevelButton *_backButton;
    NSInteger _configKind; // 0 = Environments, 1 = Settings
    EnvWindowController *_envVC;
    OS9SerratedInset *_settingInset;   // serrated border around the Settings editor (like the other panes)
    SciTextView *_settingEditor;   // JSON editor for Settings (Scintilla — JSON lexer + Platinum theme)

    BOOL _sending;
    NSTimer *_spinTimer;   // animate loading icon in the Send button
    CGFloat _spinPhase;
    NSUInteger _urlPrevLen; // previous URL length -> detect "paste" (sudden jump)
    NSTextView *_fieldEditor; // shared field editor, all auto-features disabled
    NSString *_bodyMode;    // current body mode: json | binary(file) | form-urlencoded(form)
}
@end

// Declare ALL internal methods so categories can call across files (ARC must see the selectors).
// Auto-generated from the definitions in MainWindowController*.mm.
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
- (NSString *)applyView:(const std::string &)body mode:(int)mode;   // U2: reads no ivars -> safe off main thread
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
// Incremental update (§T1): re-scan only level `parentRel` ("" = root), keeping the TreeItems
// (and already-loaded children) of other branches -> avoids re-scanning every open folder.
- (void)refreshTreeLevel:(NSString *)parentRel;
- (void)reselectTreeByRel:(NSString *)rel;   // reselect node by relPath after update (no auto-load)
- (TreeItem *)loadedFolderItemForRel:(NSString *)rel;
- (void)mergeScanLevel:(const std::string &)rel into:(NSMutableArray<TreeItem *> *)items;
// §T3: remap relPath prefix in _expandedFolders on folder rename/move to keep expansion state.
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
- (void)applyLoadedModel:(const core::RequestModel &)model rel:(NSString *)rel;  // apply loaded model (on main)
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
- (void)onStreamOpenTransport:(int)transport;
- (void)onStreamChunk:(NSString *)chunk events:(uint64_t)totalEvents;
- (void)onStreamClose:(core::StreamStatus)status code:(int)code message:(NSString *)message
               events:(uint64_t)events elapsedMs:(long long)elapsedMs truncated:(BOOL)truncated;
- (void)displayErrorKind:(core::ErrorKind)kind message:(NSString *)msg;
- (void)cacheResponseAsync:(const core::ApiResponse &)resp forId:(const std::string &)reqId;
- (void)cacheErrorAsync:(const core::ApiError &)err forId:(const std::string &)reqId;
- (void)finishSending;
- (void)startSendSpinner;
- (void)stopSendSpinner;
- (void)rebuildResponseBuffers;
- (void)rebuildResponseBuffersAsync;   // U2: format response off the main thread
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
- (void)newWs:(id)s;
- (void)newGraphQl:(id)s;
- (void)wsSendOrConnect;
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
