// Collection-tree view/model helpers; no dependency on controller ivars.
#import <Cocoa/Cocoa.h>

#include "core/domain/environment/env_config.hpp" // TreeNode (+ RequestType via request_type.hpp)

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
@property(nonatomic) BOOL childrenLoaded;   // lazy: children loaded only on expand
@property(nonatomic, strong) NSMutableArray<TreeItem *> *children;
@end

// Build ONE item from single-level metadata — NO recursion into children.
TreeItem *TreeItemFromNode(const core::TreeNode &n);

#pragma mark - DeedOutlineView (dynamic right-click context menu)

@interface DeedOutlineView : NSOutlineView
// Right-click -> controller shows its own retro overlay menu (no system NSMenu). windowPoint is the
// click location in window coords (for positioning the overlay).
@property(nonatomic, copy) void (^contextHandler)(NSInteger clickedRow, NSPoint windowPoint);
// Platinum drop feedback (AppKit's blue Aqua line is turned off): an insertion bar between rows, or
// a frame around the folder being dropped ONTO. Driven from validateDrop:.
- (void)showDropInsertAtRow:(NSInteger)row level:(NSInteger)level;
- (void)showDropOnRow:(NSInteger)row;
- (void)hideDropFeedback;
// Where a point sits inside its row, in the view's own coords: 0 = top edge, 1 = bottom edge.
// -1 when the point is not over a row.
- (CGFloat)rowFractionAtPoint:(NSPoint)p;
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
