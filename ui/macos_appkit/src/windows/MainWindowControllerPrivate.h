// Categories cannot declare ivars -> ALL ivars live in this ONE class extension, imported by every
// MainWindowController*.mm; the (Internal) interface manifests cross-file selectors so ARC sees them.
#pragma once

#import "windows/MainWindowController.h"

#import "app/AppStrings.h"
#import "app/DeedConfig.h"
#import "app/OS9Lifecycle.h"
#import "dialogs/OS9Dialog.h"
#import "windows/EnvWindowController.h"
#import "windows/TreeViews.h"
#import "windows/typeui/RequestTypeUi.h"
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
#import "widgets/OS9StyleMenu.h"
#import "widgets/OS9SerratedInset.h"
#import "widgets/OS9TitleBar.h"
#import "widgets/OS9Toast.h"
#import "widgets/OS9Toggle.h"
#import "widgets/OS9Window.h"
#import "editor/SciTextView.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/infra/serialization/field_json.hpp"   // core::serial — domain JSON field codec
#include "core/infra/import/importer.hpp"
#include "core/infra/persistence/stores.hpp"
#include "core/infra/persistence/request_naming.hpp"
#include "core/domain/environment/env_config.hpp"           // TreeNode/AppConfig/Environment/Session + RequestType
#include "core/domain/response/interaction.hpp" // StreamStatus / InteractionKind
#include "core/app/core_api_client.hpp"   // IApiClient send path
#include "bridge/UiObserver.h"            // domain ResponseEvent -> CoreResponseSink

#import "app/NsBridge.h" // N()/S() std::string <-> NSString bridges

// Conformance protocols attached to the category that implements them -> no
// -Wprotocol / -Wobjc-protocol-method-implementation warnings, while every call site that
// #imports this header sees self conform (e.g. constructing a UiObserver needs <CoreResponseSink>).
@interface MainWindowController (Tree) <NSOutlineViewDataSource, NSOutlineViewDelegate>
@end
@interface MainWindowController (Editor) <NSTextFieldDelegate, NSTextViewDelegate>
@end
@interface MainWindowController (Send) <CoreResponseSink>
@end

@interface MainWindowController () {
@protected
    // The UI talks ONLY to CoreApiClient (owns its stores/cache/senders); ALL sends (unary +
    // server-stream + WebSocket) route through IApiClient.
    std::unique_ptr<core::app::CoreApiClient> _apiClient;
    std::shared_ptr<UiObserver> _apiObserver;     // kept alive for the in-flight send
    core::domain::RequestExecutionId _apiExec;    // handle to cancel / push the in-flight send
    uint64_t _apiHandleCounter;                   // synthesizes a RequestHandle for CoreResponseSink reuse
    BOOL _apiWsActive;                            // a WebSocket session is open (send frames via it)
    // The editor's working request. Optional because core::domain::RequestModel has no default ctor
    // (identity/payload required); _hasRequest gates real use.
    std::optional<core::domain::RequestModel> _model;
    std::string _root;
    std::string _currentRel;
    std::map<std::string, std::string> _loadedBodyDrafts; // per-mode body drafts read OFF-MAIN during load
                                                          // (populateEditorsFromModel consumes; no main-thread reparse)
    // Autosave skip: last-persisted model + drafts. autosaveCurrent compares the freshly-synced state to
    // these and writes ONLY when something changed (content-based, so no missed-mutation data loss).
    std::optional<core::domain::RequestModel> _savedModel;
    std::map<std::string, std::string> _savedDrafts;
    std::string _currentId;   // id of open request (stable identifier)
    uint64_t _currentHandle;
    BOOL _hasRequest;

    // streaming — stream/WS run through IApiClient (_apiExec)
    BOOL _streaming;                       // a server-stream is in flight (Stop replaces Send)
    uint64_t _streamEvents;                // events received so far (status line)
    int64_t  _streamBytes;                 // running response size (bytes) received so far — live size counter
    uint64_t _streamToken;                 // identity of the current stream; bumped on each open, stale callbacks drop
    std::vector<core::domain::GrpcMethodDescriptor> _grpcMethods; // parallel to _servicePopup items (current request's)
    uint64_t _grpcMethodsReqSeq;  // race guard: apply only the latest listGrpcMethods result
    BOOL _grpcMethodsFetched;     // true once fetched for THIS request -> reuse, don't re-fetch (until invalidated)
    uint64_t _loadReqSeq;         // token: apply only the LATEST loadRequestAtRel model (async load)

    // GraphQL introspected schema (Schema response tab) — same lifecycle as the gRPC trio above:
    // fetched on first tab click, cached per request, invalidated on URL edit / request switch / send failure.
    NSString *_gqlSchemaSdl;      // SDL view (Pretty)
    NSString *_gqlSchemaJson;     // raw introspection JSON view (Raw)
    uint64_t _gqlSchemaReqSeq;    // race guard: apply only the latest introspectGraphQl result
    BOOL _gqlSchemaFetched;       // true once fetched for THIS request
    BOOL _gqlSchemaFetching;      // in flight -> block double-fetch on re-click

    // Chrome + containers
    NSWindow *_window;
    CALayer *_cornerMask;       // SQUARE_CORNERS=2: 9-slice non-AA mask -> pixel-rounded window corners (nil otherwise)
    CGFloat _cornerRadiusPts;   // mask radius in points (from CORNER_RADIUS_PX), re-applied on backing change
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
    // Remember active tab PER pane via a semantic KEY (title), NOT by index, NOT
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
    core::domain::ApiResponse _lastResp;
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
    OS9Toggle *_kafkaModeToggle;     // Kafka only: Producer(off)/Consumer(on) client-kind
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
    OS9BevelButton *_manageEnvButton; // Settings only: opens Environments screen
    NSInteger _configKind;       // 0 = Environments, 1 = Settings
    NSInteger _configReturnKind; // 1 = env entered from Settings -> Back returns there (0 = to main)
    EnvWindowController *_envVC;
    OS9SerratedInset *_settingInset;   // serrated border around the Settings editor (like the other panes)
    SciTextView *_settingEditor;   // JSON editor for Settings (Scintilla — JSON lexer + Platinum theme)

    BOOL _sending;
    NSTimer *_cancelWatchdog;   // Cancel escalation: cancelAll, then force-settle a core that never answers
    NSInteger _cancelStage;     // 0 = idle, 1 = cancel sent, 2 = cancelAll sent
    NSTimer *_spinTimer;   // animate loading icon in the Send button
    CGFloat _spinPhase;
    NSTimer *_liveTimer;          // live status ticker while a request is in flight (elapsed + size)
    NSTimeInterval _reqStartTime; // monotonic start (systemUptime) of the in-flight request
    NSUInteger _urlPrevLen; // previous URL length -> detect "paste" (sudden jump)
    NSTextView *_fieldEditor; // shared field editor, all auto-features disabled
    NSString *_bodyMode;    // ENABLED body mode for the open request: json | text | xml | binary(file) | form-urlencoded(form)
    // Per-mode body drafts (mode -> editor text). The model's core::Body is a tagged union that stores
    // EVERY mode's content at once (json/text/xml/form/binary) with `mode` marking the enabled one; this
    // dict mirrors that so toggling Body mode (e.g. Form<->JSON) keeps each mode's content. Rebuilt fresh
    // from the OPEN request's Body on every (re)load -> never carries content across requests.
    NSMutableDictionary<NSString *, NSString *> *_bodyDrafts;

    // Kafka Producer/Consumer drafts (mirrors _bodyDrafts): keep BOTH kinds' editor buffers (kafka-specific
    // tabs only; the trailing shared Config/timeout-tls buffer is NOT per-kind so not duplicated) and BOTH
    // kinds' last response, restored on toggle-back — flipping the client kind must not discard typed content.
    // Reset (nil'd) on every populateEditorsFromModel — never carries content across a different request.
    NSArray<NSString *> *_kafkaProducerReqBuffers; // [message, kafkaConfig] JSON
    NSArray<NSString *> *_kafkaConsumerReqBuffers; // [kafkaConfig] JSON
    NSArray<NSString *> *_kafkaProducerRespBuffers;
    NSArray<NSString *> *_kafkaConsumerRespBuffers;
    core::domain::ApiResponse _kafkaProducerLastResp;
    BOOL _kafkaProducerHasResp;
    core::domain::ApiResponse _kafkaConsumerLastResp;
    BOOL _kafkaConsumerHasResp;
}
@end

// Cross-file selector manifest: categories call these across files; ARC must see the declarations.
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
- (NSString *)applyView:(const std::string &)body mode:(int)mode;   // reads no ivars -> safe off main thread
- (void)buildStatusBar;
- (void)buildToolbar;
- (void)buildDividers;
- (void)buildConfigPane;
- (void)relayout;
- (void)layoutTabButtons:(NSArray<OS9BevelButton *> *)buttons atX:(CGFloat)x y:(CGFloat)y width:(CGFloat)width height:(CGFloat)h extra:(CGFloat)extra;
- (void)layoutConfig;
- (core::RequestType)requestType; // current request's protocol view-enum (reads domain _model; no bridge)
- (core::domain::KafkaClientKind)kafkaClientKind; // Kafka payload's Producer/Consumer mode
- (void)kafkaModeToggled:(id)sender; // toolbar toggle flipped -> rebuild _model with the other Mode alternative
// Rebuild the current gRPC payload's immutable VO via Parts + write back to _model (no-op unless gRPC).
- (void)mutateGrpc:(void (^)(core::domain::GrpcRequest::Parts &))fn;
- (void)setRequestType:(core::domain::RequestType)t;
- (void)rebuildTabButtons;
- (void)setHasRequest:(BOOL)has;
- (void)openFolder:(id)sender;
- (core::AppConfig)appDefaultsFromEnv;
- (void)openCollectionRoot:(NSString *)path;
- (BOOL)resyncCurrentRelById;
- (void)loadChildrenOf:(TreeItem *)folder;
- (void)reloadTree;
- (void)restoreExpansion:(NSArray<TreeItem *> *)items;
// Incremental update: re-scan only level `parentRel` ("" = root), keeping the TreeItems
// (and already-loaded children) of other branches -> avoids re-scanning every open folder.
- (void)refreshTreeLevel:(NSString *)parentRel;
- (void)reselectTreeByRel:(NSString *)rel;   // reselect node by relPath after update (no auto-load)
- (TreeItem *)loadedFolderItemForRel:(NSString *)rel;
- (BOOL)mergeScanLevel:(const std::string &)rel into:(NSMutableArray<TreeItem *> *)items;
// Remap relPath prefix in _expandedFolders on folder rename/move to keep expansion state.
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
- (void)applyLoadedModel:(const core::domain::RequestModel &)model rel:(NSString *)rel;  // apply loaded model (on main)
- (void)showCachedResponseForId:(const std::string &)reqId;
- (void)populateEditorsFromModel;
- (NSInteger)tabIndexForKey:(NSString *)key inTitles:(NSArray<NSString *> *)titles;
- (void)stashActiveReqBuffer;
- (BOOL)syncModelFromEditors:(BOOL)silent;
- (void)closeEditorIfDeleted:(NSArray<TreeItem *> *)deleted;
- (std::map<std::string, std::string>)collectBodyDrafts;
- (void)autosaveCurrent;
- (NSString *)bodyButtonTitle;
- (NSInteger)bodyTabIndex;
- (void)updateBodyButtonLabel;
- (NSString *)bodyTemplateForMode:(NSString *)mode;
- (void)bodyButtonClicked:(OS9BevelButton *)b;
- (void)pickBodyMode:(NSString *)mode;
- (void)reqTabClicked:(OS9BevelButton *)b;
- (void)selectReqTab:(NSInteger)tab;
- (void)respTabClicked:(OS9BevelButton *)b;
- (void)highlightActiveTab:(NSArray<OS9BevelButton *> *)buttons active:(NSInteger)active;
- (void)prettyToggle:(id)sender;
- (void)applyPrettyToFocusedPane;
- (void)copyAsCurl:(id)sender;
- (void)copyCurlForRel:(NSString *)rel;   // right-click "Copy as cURL" on a tree request
- (void)zoomToggle:(id)sender;
- (void)collapseToggle:(id)sender;
- (void)applyConfiguredFontAndRefresh;
- (void)controlTextDidChange:(NSNotification *)note;
- (void)importNow:(NSString *)text kind:(core::domain::ImportKind)kind;
- (void)offerImport:(NSString *)text kind:(core::domain::ImportKind)kind;
- (void)applyImport:(const core::domain::RequestModel &)m;
- (void)restoreUrlField;
- (void)methodChanged:(id)sender;
- (void)urlCommitted:(id)sender;
- (NSString *)urlDecodeComponent:(NSString *)s;
- (void)parseUrlQueryIntoQueryTab;
- (void)updateTitle;
- (void)saveRequest:(id)sender;
- (void)sendRequest:(id)sender;
- (void)sendViaApiClient;     // route unary send through IApiClient
- (void)streamViaApiClient;   // route server-stream (gRPC/SSE) through IApiClient
- (void)wsViaApiClient;       // route WebSocket connect/send-frame through IApiClient
- (void)cancelClicked:(id)sender;
- (void)armCancelWatchdog;    // escalate + force-settle so Cancel is never a no-op
- (void)stopCancelWatchdog;
- (void)onCoreResponse:(uint64_t)handle response:(const core::domain::ApiResponse &)resp;
- (void)onCoreError:(uint64_t)handle error:(const core::domain::ApiError &)err;
- (void)onStreamOpenTransport:(int)transport;
- (void)onStreamChunk:(NSString *)chunk events:(uint64_t)totalEvents;
- (void)onStreamClose:(core::StreamStatus)status code:(int)code message:(NSString *)message
               events:(uint64_t)events elapsedMs:(long long)elapsedMs truncated:(BOOL)truncated;
- (void)displayErrorKind:(core::domain::ErrorKind)kind message:(NSString *)msg elapsedMs:(long long)elapsedMs;
- (void)cacheResponseAsync:(const core::domain::ApiResponse &)resp forId:(const std::string &)reqId;
- (void)cacheErrorAsync:(const core::domain::ApiError &)err forId:(const std::string &)reqId;
- (void)finishSending;
- (void)startSendSpinner;
- (void)stopSendSpinner;
- (void)beginRequestTiming;   // reset live counters + start the live status ticker (elapsed/size)
- (void)stopLiveTimer;
- (void)liveTick;
- (NSString *)humanSize:(int64_t)bytes;   // "1.2kb" / "345b"
- (NSString *)elapsedText:(long long)ms;  // "850ms" / "1.23s" (>1000ms -> seconds)
- (long long)measuredElapsedMs;           // UI's own monotonic elapsed (for cancel: real time, not 0ms)
- (void)rebuildResponseBuffers;
- (void)rebuildResponseBuffersAsync;   // format response off the main thread
- (NSArray<NSString *> *)computeResponseBuffersFor:(const core::domain::ApiResponse &)r
                                              type:(core::RequestType)type
                                        prettyMode:(int)prettyMode;
- (void)applyResponseBuffers:(NSArray<NSString *> *)bufs;
- (void)updateStatus:(NSString *)text;
- (NSString *)clockFromEpochMs:(int64_t)ms;
- (void)updateStatusFromResponse:(const core::domain::ApiResponse &)r error:(BOOL)isErr endMs:(int64_t)endMs;
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
- (void)applyGrpcMethods:(const std::vector<core::domain::GrpcMethodDescriptor> &)methods error:(NSString *)err                 openMenu:(BOOL)openMenu;
- (void)serviceMethodChanged:(id)sender;
- (void)applySelectedGrpcMethod:(NSInteger)idx;
- (void)applyReqPaneLanguage;                       // XML for the SOAP Envelope tab, JSON otherwise
- (void)applyRespPaneLanguageFor:(NSString *)content; // content-sniff: leading '<' -> XML
- (void)invalidateGqlSchema;
- (BOOL)respActiveTabIsSchema;
- (void)displayGqlSchemaPane;
- (void)fetchGqlSchema;
- (void)applyGqlSchema:(NSString *)sdl json:(NSString *)json error:(NSString *)err;
- (void)manageEnvClicked:(id)sender;
- (void)refreshVarsForSend;
- (BOOL)saveSettingsFromEditor;
- (void)showContextMenuForRow:(NSInteger)row atWindowPoint:(NSPoint)pt;
- (void)purgeCacheAtRel:(NSString *)rel isFolder:(BOOL)isFolder;
- (void)deleteSelectedMulti:(id)sender;
- (std::string)selectedFolderRel;
- (void)wsSendOrConnect;
- (void)createRequest:(core::RequestType)t name:(NSString *)name;
- (void)newFolder:(id)s;
- (void)renameSel:(id)s;
- (void)dupSel:(id)s;
- (void)deleteSel:(id)s;
- (void)closeWindow:(id)sender;
- (BOOL)windowShouldClose:(NSWindow *)sender;
- (void)windowDidResize:(NSNotification *)note;
- (void)applyPixelCorners;   // SQUARE_CORNERS=2: (re)apply the 9-slice pixel-corner mask
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
