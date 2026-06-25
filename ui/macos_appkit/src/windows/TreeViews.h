// TreeViews — view/model helpers for MainWindowController's collection tree (split from controller).
// TreeItem (NSOutlineView model), DeedOutlineView (context menu), TreeCellView (retro icon),
// OS9RowView (gray highlight). No dependency on controller ivars -> stands alone.
#import <Cocoa/Cocoa.h>

#include "core/types.hpp"

// Pasteboard type for drag-and-drop moving of a request within the tree.
extern NSString *const kTreeDragType;

#pragma mark - TreeItem (model for NSOutlineView)

@interface TreeItem : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *relPath;
@property(nonatomic, copy) NSString *requestId;   // stable id of the request
@property(nonatomic) BOOL isFolder;
@property(nonatomic, copy) NSString *badge;
@property(nonatomic, copy) NSString *mark;   // line-leading marker: HTTP method, or "gRPC"
@property(nonatomic) BOOL grpc;
@property(nonatomic) core::RequestType requestType;   // request kind (folders: unused) — drives cURL menu item
@property(nonatomic) BOOL childrenLoaded;   // lazy: children loaded only on expand (§3)
@property(nonatomic, strong) NSMutableArray<TreeItem *> *children;
@end

// Build ONE item from single-level metadata — NO recursion into children (lazy §3).
TreeItem *TreeItemFromNode(const core::TreeNode &n);

#pragma mark - DeedOutlineView (dynamic right-click context menu)

@interface DeedOutlineView : NSOutlineView
// Right-click -> controller shows its own retro overlay menu (no system NSMenu). windowPoint is the
// click location in window coords (for positioning the overlay).
@property(nonatomic, copy) void (^contextHandler)(NSInteger clickedRow, NSPoint windowPoint);
@end

#pragma mark - TreeCellView (self-drawn retro folder/doc icon)

@interface TreeCellView : NSView
@property(nonatomic, copy) NSString *text;   // folder: name; request: name (without method)
@property(nonatomic, copy) NSString *mark;   // request: method type (GET/POST/gRPC) — drawn in fixed left column
@property(nonatomic) BOOL isFolder;
@property(nonatomic) BOOL isExpanded;   // folder open -> ▽ triangle; closed -> ▷ (drawn only for folders)
@end

#pragma mark - OS9RowView (self-drawn GRAY highlight, drops default blue)

@interface OS9RowView : NSTableRowView
@end
