// TreeViews — view/model phụ cho cây collection của MainWindowController (tách khỏi controller).
// TreeItem (model NSOutlineView), DeedOutlineView (context menu), TreeCellView (icon retro),
// OS9RowView (highlight xám). Không phụ thuộc ivar controller -> đứng riêng được.
#import <Cocoa/Cocoa.h>

#include "core/types.hpp"

// Pasteboard type cho kéo-thả di chuyển request trong cây.
extern NSString *const kTreeDragType;

#pragma mark - TreeItem (model cho NSOutlineView)

@interface TreeItem : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *relPath;
@property(nonatomic, copy) NSString *requestId;   // id ổn định của request
@property(nonatomic) BOOL isFolder;
@property(nonatomic, copy) NSString *badge;
@property(nonatomic, copy) NSString *mark;   // mốc đầu dòng: HTTP method, hoặc "gRPC"
@property(nonatomic) BOOL grpc;
@property(nonatomic) BOOL childrenLoaded;   // lazy: con chỉ nạp khi expand (§3)
@property(nonatomic, strong) NSMutableArray<TreeItem *> *children;
@end

// Dựng MỘT item từ metadata 1 cấp — KHÔNG đệ quy vào con (lazy §3).
TreeItem *TreeItemFromNode(const core::TreeNode &n);

#pragma mark - DeedOutlineView (context menu động chuột phải)

@interface DeedOutlineView : NSOutlineView
@property(nonatomic, copy) NSMenu *(^menuProvider)(NSInteger clickedRow);
@end

#pragma mark - TreeCellView (icon thư mục/doc retro tự vẽ)

@interface TreeCellView : NSView
@property(nonatomic, copy) NSString *text;   // folder: tên; request: tên (không kèm method)
@property(nonatomic, copy) NSString *mark;   // request: method type (GET/POST/gRPC) — vẽ ở cột cố định trái
@property(nonatomic) BOOL isFolder;
@property(nonatomic) BOOL isExpanded;   // folder mở -> tam giác ▽; đóng -> ▷ (chỉ vẽ cho folder)
@end

#pragma mark - OS9RowView (highlight XÁM tự vẽ, bỏ xanh mặc định)

@interface OS9RowView : NSTableRowView
@end
