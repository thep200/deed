// AppStrings.mm — definitions for AppStrings.h. Edit displayed wording HERE.
#import "app/AppStrings.h"

#pragma mark - Shared buttons / labels
NSString *const StrOK = @"OK";
NSString *const StrCancel = @"Cancel";
NSString *const StrDelete = @"Delete";
NSString *const StrRename = @"Rename";
NSString *const StrDuplicate = @"Duplicate";

#pragma mark - Menu
NSString *const StrMenuQuit = @"Quit deed";
NSString *const StrMenuFile = @"File";
NSString *const StrMenuEdit = @"Edit";
NSString *const StrMenuWindow = @"Window";
NSString *const StrMenuUndo = @"Undo";
NSString *const StrMenuRedo = @"Redo";
NSString *const StrMenuCut = @"Cut";
NSString *const StrMenuCopy = @"Copy";
NSString *const StrMenuPaste = @"Paste";
NSString *const StrMenuSelectAll = @"Select All";
NSString *const StrMenuMinimize = @"Minimize";
NSString *const StrMenuZoom = @"Zoom";
NSString *const StrMenuClose = @"Close";
NSString *const StrMenuSend = @"Send";

#pragma mark - Toolbar / main buttons
NSString *const StrOpenFolder = @"Open Folder…";
NSString *const StrOpenCollection = @"Open Collection";
NSString *const StrSave = @"Save";
NSString *const StrBtnCurl = @"cURL";
NSString *const StrBtnBack = @"Back";
NSString *const StrEnvLocal = @"Local";
NSString *const StrEnvManage = @"Manage…";

#pragma mark - Tooltip
NSString *const StrTipCurl = @"Copy current request as cURL";
NSString *const StrTipSettings = @"Settings";
NSString *const StrTipSend = @"Send  ⌘↩";
NSString *const StrTipProtoSource = @"Proto source: Reflection (ask server) or load a .proto file";
NSString *const StrTipPretty = @"Pretty/Raw/Encode/Decode — applies to the focused pane";

#pragma mark - Placeholder
NSString *const StrPhUrl = @"localhost:8000/api/deed";
NSString *const StrPhName = @"Name";

#pragma mark - View mode
NSString *const StrViewPretty = @"Pretty";
NSString *const StrViewRaw = @"Raw";
NSString *const StrViewEncode = @"Encode";
NSString *const StrViewDecode = @"Decode";

#pragma mark - Proto source (gRPC)
NSString *const StrProtoReflection = @"Reflection";
NSString *const StrProtoFile = @".proto";

#pragma mark - HTTP method
NSString *const StrMethodGet = @"GET";
NSString *const StrMethodPost = @"POST";
NSString *const StrMethodPut = @"PUT";
NSString *const StrMethodPatch = @"PATCH";
NSString *const StrMethodDelete = @"DELETE";
NSString *const StrMethodHead = @"HEAD";
NSString *const StrMethodOptions = @"OPTIONS";

#pragma mark - RPC picker
NSString *const StrNoRpc = @"No RPC";
NSString *const StrLoading = @"Loading...";

#pragma mark - Tab
NSString *const StrTabBody = @"Body";
NSString *const StrTabQuery = @"Query";
NSString *const StrTabHeaders = @"Headers";
NSString *const StrTabAuth = @"Auth";
NSString *const StrTabResponse = @"Response";
NSString *const StrTabRequest = @"Request";
NSString *const StrTabCookie = @"Cookie";
NSString *const StrTabMessage = @"Message";
NSString *const StrTabMetadata = @"Metadata";

#pragma mark - Body mode
NSString *const StrBodyJson = @"JSON";
NSString *const StrBodyFile = @"File";
NSString *const StrBodyForm = @"Form";

#pragma mark - Config screen titles
NSString *const StrTitleSettings = @"Settings";
NSString *const StrTitleEnvironments = @"Environments";

#pragma mark - Tree context menu
NSString *const StrMenuNewHttp = @"New HTTP Request";
NSString *const StrMenuNewGrpc = @"New gRPC Request";
NSString *const StrNewFolder = @"New Folder";
NSString *const StrFmtDeleteItems = @"Delete %lu items";

#pragma mark - Default names
NSString *const StrDefaultRequestName = @"New Request";
NSString *const StrDefaultRpcName = @"New RPC";
NSString *const StrDefaultImportName = @"request";
NSString *const StrImportedGrpc = @"Imported gRPC";

#pragma mark - Dialog: Rename / Import
NSString *const StrDlgRenameMsg = @"New name:";
NSString *const StrDlgImportTitle = @"Import";
NSString *const StrBtnReplaceCurrent = @"Replace current";
NSString *const StrBtnCreateRequest = @"Create request";
NSString *const StrCurlDetected = @"cURL command detected";
NSString *const StrGrpcurlDetected = @"grpcurl command detected";

#pragma mark - Dialog: delete (confirm)
NSString *const StrDlgDeleteEnv = @"Delete environment";
NSString *const StrDlgDeleteAlias = @"Delete alias";
NSString *const StrFmtConfirmDelete = @"Do you want to delete \"%@\"?";
NSString *const StrFmtConfirmDeleteMulti = @"Do you want to delete %lu selected items?";
NSString *const StrFmtConfirmDeleteEnv = @"Do you want to delete environment \"%@\"?";
NSString *const StrFmtConfirmDeleteAlias = @"Do you want to delete alias \"%@\"?";

#pragma mark - Dialog: Environments grid
NSString *const StrDlgInvalidTitle = @"Invalid";
NSString *const StrGridAlias = @"Alias";
NSString *const StrGridAddAlias = @"Add alias";
NSString *const StrDlgRenameAlias = @"Rename alias";
NSString *const StrDlgRenameEnv = @"Rename environment";
NSString *const StrDlgNewEnv = @"New environment";
NSString *const StrDlgNewAlias = @"New alias";
NSString *const StrFmtEnvAliasTitle = @"%@ · %@";

#pragma mark - Validate (names)
NSString *const StrValNameEmpty = @"Name cannot be empty";
NSString *const StrValAliasExists = @"Alias already exists";
NSString *const StrValEnvExists = @"Environment already exists";
NSString *const StrValReservedBase = @"This name is reserved for the base column";
NSString *const StrFmtAliasExists = @"Alias \"%@\" already exists.";
NSString *const StrFmtEnvExists = @"Environment \"%@\" already exists.";

#pragma mark - Toast / status
NSString *const StrToastOpenFolderFirst = @"Open a collection folder first";
NSString *const StrToastUnaryOnly = @"POC supports unary gRPC only";
NSString *const StrToastCopiedCurl = @"Copied as cURL";
NSString *const StrToastSaved = @"Saved";
NSString *const StrToastRenamed = @"Renamed";
NSString *const StrToastDuplicated = @"Duplicated";
NSString *const StrToastInvalidSettings = @"Invalid settings JSON — skipped";
NSString *const StrToastAutosaveFailed = @"Autosave failed: invalid JSON";
NSString *const StrToastRequestGone = @"Request no longer exists";
NSString *const StrToastEnterGrpcHost = @"Enter gRPC host first (e.g. localhost:50051)";
NSString *const StrStatusCancelled = @"Cancelled";
NSString *const StrStatusNetworkError = @"NETWORK ERROR";

NSString *const StrGrpcTagUnary = @"[Unary]";
NSString *const StrGrpcTagServerStream = @"[S-> C]";
NSString *const StrGrpcTagClientStream = @"[S <-C]";
NSString *const StrGrpcTagBidiStream = @"[S<->C]";

NSString *const StrFmtStreamReceived = @"Received %llu events";
NSString *const StrFmtStreamOk = @"OK%@ | %llu events | %lldms";
NSString *const StrStreamTruncated = @" (truncated)";
NSString *const StrFmtStreamCancelled = @"%@ | %llu events";
NSString *const StrFmtStreamError = @"%@ | %d | %@";
NSString *const StrStreamKindError = @"Error";
NSString *const StrStreamKindTimeout = @"Timeout";
NSString *const StrNoSetCookie = @"(no Set-Cookie)";
NSString *const StrFmtToastEnv = @"ENV: %@";
NSString *const StrFmtToastNotFound = @"Not found: %s (skipped)";
NSString *const StrFmtToastInvalidJsonTab = @"Invalid JSON in tab %@: %s";
NSString *const StrFmtToastImportFailed = @"Import %@ failed: %s";
NSString *const StrFmtToastImportedCreated = @"Imported & created: %@";
NSString *const StrFmtToastReplaced = @"Replaced current request (%@)";
NSString *const StrFmtToastListRpcs = @"List RPCs: %@";
NSString *const StrFmtToastCreated = @"Created: %@";
NSString *const StrFmtVarRenamed = @"Variable renamed; requests using {{%@}} must be updated manually.";
