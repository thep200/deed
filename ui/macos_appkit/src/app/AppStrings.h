// AppStrings.h — THE SINGLE SOURCE for every user-facing text string in the app.
//
// Goal: gather all text (button labels, dialog titles/bodies, toasts, menus, tabs,
// placeholders, error/validation messages, default names...) in ONE place for easy
// management, review, and consistent wording.
//
// Conventions:
//  - Constants `extern NSString *const Str...` (defined in AppStrings.mm) -> one linked copy.
//  - Constants prefixed `StrFmt...` are FORMAT strings (used with +stringWithFormat:);
//    keep argument order/types intact (%@, %lu, %s...).
//  - Identical strings with the same meaning SHARE one constant (e.g. Cancel/OK/Delete).
//
// Intentionally excluded: internal keys/identifiers (mode "json", key "Global"...),
// JSON data templates, icon glyphs, and pure-symbol format fragments (e.g. "%@ | %@").
#pragma once

#import <Foundation/Foundation.h>

#pragma mark - Shared buttons / labels
extern NSString *const StrOK;
extern NSString *const StrCancel;
extern NSString *const StrDelete;
extern NSString *const StrRename;
extern NSString *const StrDuplicate;

#pragma mark - Menu (main.mm)
extern NSString *const StrMenuQuit;
extern NSString *const StrMenuFile;
extern NSString *const StrMenuEdit;
extern NSString *const StrMenuWindow;
extern NSString *const StrMenuUndo;
extern NSString *const StrMenuRedo;
extern NSString *const StrMenuCut;
extern NSString *const StrMenuCopy;
extern NSString *const StrMenuPaste;
extern NSString *const StrMenuSelectAll;
extern NSString *const StrMenuMinimize;
extern NSString *const StrMenuZoom;
extern NSString *const StrMenuClose;
extern NSString *const StrMenuSend;

#pragma mark - Toolbar / main buttons
extern NSString *const StrOpenFolder;   // File menu + open-folder button
extern NSString *const StrOpenCollection;   // button prompt in NSOpenPanel
extern NSString *const StrSave;
extern NSString *const StrBtnBack;
extern NSString *const StrEnvLocal;      // base environment column/variable label
extern NSString *const StrEnvManage;

#pragma mark - Tooltip
extern NSString *const StrTipSettings;
extern NSString *const StrTipSend;
extern NSString *const StrTipProtoSource;
extern NSString *const StrTipGrpcTls;
extern NSString *const StrTipPretty;

#pragma mark - Placeholder
extern NSString *const StrPhUrl;
extern NSString *const StrPhName;

#pragma mark - View mode (Pretty/Raw/Encode/Decode)
extern NSString *const StrViewPretty;
extern NSString *const StrViewRaw;
extern NSString *const StrViewEncode;
extern NSString *const StrViewDecode;

#pragma mark - Proto source (gRPC)
extern NSString *const StrProtoReflection;
extern NSString *const StrProtoFile;
extern NSString *const StrTls;

#pragma mark - HTTP method
extern NSString *const StrMethodGet;
extern NSString *const StrMethodPost;
extern NSString *const StrMethodPut;
extern NSString *const StrMethodPatch;
extern NSString *const StrMethodDelete;
extern NSString *const StrMethodHead;
extern NSString *const StrMethodOptions;

#pragma mark - RPC picker
extern NSString *const StrNoRpc;
extern NSString *const StrLoading;
extern NSString *const StrFetching;

#pragma mark - Tab (request/response)
extern NSString *const StrTabBody;
extern NSString *const StrTabQuery;
extern NSString *const StrTabHeaders;
extern NSString *const StrTabAuth;
extern NSString *const StrTabResponse;
extern NSString *const StrTabRequest;
extern NSString *const StrTabCookie;
extern NSString *const StrTabMessage;
extern NSString *const StrTabGqlQuery;
extern NSString *const StrTabVariables;
extern NSString *const StrTabMetadata;
extern NSString *const StrTabConfig;
extern NSString *const StrTabSchema; // GraphQL response pane: introspected server schema
extern NSString *const StrTabEnvelope; // SOAP: full XML envelope editor
extern NSString *const StrTabSoap;     // SOAP: {action, version} config

#pragma mark - Kafka client-kind toggle (SPEC_kafka §2.0)
extern NSString *const StrKafkaProducer;
extern NSString *const StrKafkaConsumer;
// Kafka-specific settings tab (topic/ack/compression.../topics/group/...) — DISTINCT from the shared
// per-request StrTabConfig (timeout_ms/tls) that every request type also gets, appended last (§ see
// populateEditorsFromModel / syncModelFromEditors's "last buffer = shared RequestConfig" invariant).
extern NSString *const StrTabKafkaConfig;

#pragma mark - Body mode (dropdown)
extern NSString *const StrBodyJson;
extern NSString *const StrBodyFile;
extern NSString *const StrBodyForm;
extern NSString *const StrBodyText;
extern NSString *const StrBodyXml;

#pragma mark - Config screen titles
extern NSString *const StrTitleSettings;
extern NSString *const StrTitleEnvironments;

#pragma mark - Tree context menu
extern NSString *const StrMenuNewHttp;
extern NSString *const StrMenuNewGrpc;
extern NSString *const StrMenuNewWs;
extern NSString *const StrMenuNewGraphQl;
extern NSString *const StrMenuNewKafka;
extern NSString *const StrMenuNewSoap;
extern NSString *const StrNewFolder;     // menu + default folder name
extern NSString *const StrMenuCopyCurl;  // right-click: copy request as cURL/grpcurl
extern NSString *const StrFmtDeleteItems;

#pragma mark - Default names
extern NSString *const StrDefaultRequestName;
extern NSString *const StrDefaultRpcName;
extern NSString *const StrDefaultWsName;
extern NSString *const StrDefaultGqlName;
extern NSString *const StrDefaultKafkaName;
extern NSString *const StrDefaultSoapName;
extern NSString *const StrDefaultImportName;   // fallback when a name cannot be inferred
extern NSString *const StrImportedGrpc;

#pragma mark - Dialog: Rename / Import
extern NSString *const StrDlgRenameMsg;
extern NSString *const StrDlgImportTitle;
extern NSString *const StrBtnReplaceCurrent;
extern NSString *const StrBtnCreateRequest;
extern NSString *const StrCurlDetected;
extern NSString *const StrGrpcurlDetected;
extern NSString *const StrGraphqlDetected;

#pragma mark - Dialog: delete (confirm)
extern NSString *const StrDlgDeleteEnv;            // delete-environment dialog title
extern NSString *const StrDlgDeleteAlias;          // delete-alias dialog title
extern NSString *const StrFmtConfirmDelete;        // "Do you want to delete \"%@\"?"
extern NSString *const StrFmtConfirmDeleteMulti;   // "Do you want to delete %lu selected items?"
extern NSString *const StrFmtConfirmDeleteEnv;     // "Do you want to delete environment \"%@\"?"
extern NSString *const StrFmtConfirmDeleteAlias;   // "Do you want to delete alias \"%@\"?"

#pragma mark - Dialog: Environments grid
extern NSString *const StrDlgInvalidTitle;
extern NSString *const StrGridAlias;
extern NSString *const StrGridAddAlias;
extern NSString *const StrDlgRenameAlias;
extern NSString *const StrDlgRenameEnv;
extern NSString *const StrDlgNewEnv;
extern NSString *const StrDlgNewAlias;
extern NSString *const StrFmtEnvAliasTitle;

#pragma mark - Validate (names)
extern NSString *const StrValNameEmpty;
extern NSString *const StrValAliasExists;
extern NSString *const StrValEnvExists;
extern NSString *const StrValReservedBase;
extern NSString *const StrFmtAliasExists;   // duplicate-alias dialog (with name)
extern NSString *const StrFmtEnvExists;     // duplicate-env dialog (with name)

#pragma mark - Toast / status
extern NSString *const StrToastOpenFolderFirst;
extern NSString *const StrToastUnaryOnly;
extern NSString *const StrToastWsEmptyFrame;
extern NSString *const StrToastWsQueueFull;
extern NSString *const StrToastCopiedCurl;
extern NSString *const StrToastSaved;
extern NSString *const StrToastRenamed;
extern NSString *const StrToastDuplicated;
extern NSString *const StrToastInvalidSettings;
extern NSString *const StrToastAutosaveFailed;
extern NSString *const StrToastRequestGone;
extern NSString *const StrToastEnterGrpcHost;
extern NSString *const StrToastEnterGqlUrl;
extern NSString *const StrFetchingSchema;
extern NSString *const StrStatusCancelled;
extern NSString *const StrStatusNetworkError;

// --- gRPC streaming: RPC picker tags (streaming direction) ---
extern NSString *const StrGrpcTagUnary;          // [Unary]
extern NSString *const StrGrpcTagServerStream;   // [S-> C]
extern NSString *const StrGrpcTagClientStream;   // [S <-C]
extern NSString *const StrGrpcTagBidiStream;     // [S<->C]

// --- gRPC streaming: status line (fields separated by '|') ---
extern NSString *const StrFmtReqElapsed;         // live elapsed only (unary in-flight)
extern NSString *const StrFmtStreamLive;         // live elapsed | size | events (streaming in-flight)
extern NSString *const StrFmtStreamOk;           // "OK%@ | %@ | %llu events | %lldms"  (trunc | size | events | ms)
extern NSString *const StrStreamTruncated;       // " (truncated)"
extern NSString *const StrFmtStreamCancelled;    // "%@ | %@ | %llu events"  (Cancelled | size | events)
extern NSString *const StrFmtStreamError;        // "%@ | %d | %@"  (kind | code | message)
extern NSString *const StrStreamKindError;       // Error
extern NSString *const StrStreamKindTimeout;     // Timeout
extern NSString *const StrNoSetCookie;
extern NSString *const StrFmtToastEnv;
extern NSString *const StrFmtToastNotFound;
extern NSString *const StrFmtToastInvalidJsonTab;
extern NSString *const StrFmtToastImportFailed;
extern NSString *const StrFmtToastImportedCreated;
extern NSString *const StrFmtToastReplaced;
extern NSString *const StrFmtToastListRpcs;
extern NSString *const StrFmtToastFetchSchema;
extern NSString *const StrFmtToastCreated;
extern NSString *const StrFmtVarRenamed;
