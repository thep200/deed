// AppStrings.h — NGUỒN DUY NHẤT cho mọi chuỗi text hiển thị cho người dùng trong app.
//
// Mục tiêu: gom toàn bộ text (nhãn nút, tiêu đề/nội dung dialog, toast, menu, tab,
// placeholder, thông báo lỗi/validate, tên mặc định...) vào MỘT chỗ để dễ quản lý,
// rà soát và đổi từ ngữ nhất quán.
//
// Quy ước:
//  - Hằng dạng `extern NSString *const Str...` (định nghĩa ở AppStrings.mm) -> link 1 bản.
//  - Hằng có tiền tố `StrFmt...` là chuỗi ĐỊNH DẠNG (dùng với +stringWithFormat:),
//    giữ nguyên thứ tự/loại tham số (%@, %lu, %s...).
//  - Chuỗi giống hệt & cùng ngữ nghĩa thì DÙNG CHUNG một hằng (vd Cancel/OK/Delete).
//
// Không gom vào đây (cố ý): khoá nội bộ/định danh (mode "json", key "Global"...),
// template dữ liệu JSON, glyph icon, và các mảnh format thuần ký hiệu (vd "%@ | %@").
#pragma once

#import <Foundation/Foundation.h>

#pragma mark - Nút / nhãn dùng chung
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

#pragma mark - Toolbar / nút chính
extern NSString *const StrOpenFolder;   // menu File + nút mở thư mục
extern NSString *const StrOpenCollection;   // prompt nút trong NSOpenPanel
extern NSString *const StrSave;
extern NSString *const StrBtnCurl;
extern NSString *const StrBtnBack;
extern NSString *const StrEnvLocal;      // nhãn cột/biến môi trường base
extern NSString *const StrEnvManage;

#pragma mark - Tooltip
extern NSString *const StrTipCurl;
extern NSString *const StrTipSettings;
extern NSString *const StrTipSend;
extern NSString *const StrTipProtoSource;
extern NSString *const StrTipPretty;

#pragma mark - Placeholder
extern NSString *const StrPhUrl;
extern NSString *const StrPhName;

#pragma mark - View mode (Pretty/Raw/Encode/Decode)
extern NSString *const StrViewPretty;
extern NSString *const StrViewRaw;
extern NSString *const StrViewEncode;
extern NSString *const StrViewDecode;

#pragma mark - Nguồn proto (gRPC)
extern NSString *const StrProtoReflection;
extern NSString *const StrProtoFile;

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

#pragma mark - Tab (request/response)
extern NSString *const StrTabBody;
extern NSString *const StrTabQuery;
extern NSString *const StrTabHeaders;
extern NSString *const StrTabAuth;
extern NSString *const StrTabResponse;
extern NSString *const StrTabRequest;
extern NSString *const StrTabCookie;
extern NSString *const StrTabMessage;
extern NSString *const StrTabMetadata;

#pragma mark - Body mode (dropdown)
extern NSString *const StrBodyJson;
extern NSString *const StrBodyFile;
extern NSString *const StrBodyForm;

#pragma mark - Tiêu đề màn config
extern NSString *const StrTitleSettings;
extern NSString *const StrTitleEnvironments;

#pragma mark - Context menu cây
extern NSString *const StrMenuNewHttp;
extern NSString *const StrMenuNewGrpc;
extern NSString *const StrNewFolder;     // menu + tên folder mặc định
extern NSString *const StrFmtDeleteItems;

#pragma mark - Tên mặc định
extern NSString *const StrDefaultRequestName;
extern NSString *const StrDefaultRpcName;
extern NSString *const StrDefaultImportName;   // fallback khi không suy ra được tên
extern NSString *const StrImportedGrpc;

#pragma mark - Dialog: Rename / Import
extern NSString *const StrDlgRenameMsg;
extern NSString *const StrDlgImportTitle;
extern NSString *const StrBtnReplaceCurrent;
extern NSString *const StrBtnCreateRequest;
extern NSString *const StrCurlDetected;
extern NSString *const StrGrpcurlDetected;

#pragma mark - Dialog: xoá (xác nhận)
extern NSString *const StrDlgDeleteEnv;            // tiêu đề dialog xoá environment
extern NSString *const StrDlgDeleteAlias;          // tiêu đề dialog xoá alias
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

#pragma mark - Validate (tên)
extern NSString *const StrValNameEmpty;
extern NSString *const StrValAliasExists;
extern NSString *const StrValEnvExists;
extern NSString *const StrValReservedBase;
extern NSString *const StrFmtAliasExists;   // dialog báo trùng alias (có tên)
extern NSString *const StrFmtEnvExists;     // dialog báo trùng env (có tên)

#pragma mark - Toast / status
extern NSString *const StrToastOpenFolderFirst;
extern NSString *const StrToastUnaryOnly;
extern NSString *const StrToastCopiedCurl;
extern NSString *const StrToastSaved;
extern NSString *const StrToastRenamed;
extern NSString *const StrToastDuplicated;
extern NSString *const StrToastInvalidSettings;
extern NSString *const StrToastAutosaveFailed;
extern NSString *const StrToastRequestGone;
extern NSString *const StrToastEnterGrpcHost;
extern NSString *const StrStatusCancelled;
extern NSString *const StrStatusNetworkError;
extern NSString *const StrNoSetCookie;
extern NSString *const StrFmtToastEnv;
extern NSString *const StrFmtToastNotFound;
extern NSString *const StrFmtToastInvalidJsonTab;
extern NSString *const StrFmtToastImportFailed;
extern NSString *const StrFmtToastImportedCreated;
extern NSString *const StrFmtToastReplaced;
extern NSString *const StrFmtToastListRpcs;
extern NSString *const StrFmtToastCreated;
extern NSString *const StrFmtVarRenamed;
