#import "windows/TreeViews.h"

#import "theme/OS9Theme.h"

#include <string>

NSString *const kTreeDragType = @"com.example.deed.request";

static inline NSString *N(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }

@implementation TreeItem
@end

TreeItem *TreeItemFromNode(const core::TreeNode &n) {
    TreeItem *it = [TreeItem new];
    it.name = N(n.name);
    it.relPath = N(n.relPath);
    it.isFolder = n.isFolder;
    it.requestId = N(n.id);
    it.children = [NSMutableArray array];
    it.childrenLoaded = NO;
    if (!n.isFolder) {
        it.grpc = (n.requestType == core::RequestType::Grpc);
        it.badge = [NSString stringWithFormat:@"%s %s", core::toString(n.requestType).c_str(), n.methodOrType.c_str()];
        // HTTP -> tên method (GET/POST...); gRPC -> "gRPC".
        it.mark = it.grpc ? @"gRPC" : N(n.methodOrType);
    }
    return it;
}

@implementation DeedOutlineView
- (NSMenu *)menuForEvent:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    return self.menuProvider ? self.menuProvider(row) : nil;
}
// Bỏ mũi tên fold (disclosure triangle) — folder luôn mở sẵn.
- (NSRect)frameOfOutlineCellAtRow:(NSInteger)row { return NSZeroRect; }
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

@implementation OS9RowView
// Tô nền xám nhẹ khi row được chọn (đơn HOẶC nhiều); KHÔNG đảo chữ trắng.
- (void)drawBackgroundInRect:(NSRect)dirtyRect {
    [[NSColor whiteColor] set];
    NSRectFill(self.bounds);
    if (self.selected) { [[OS9Theme rowSelectionGray] set]; NSRectFill(self.bounds); }
}
// Vô hiệu mọi hiệu ứng selection/emphasized mặc định (không vẽ xanh ở bất kỳ trạng thái nào).
- (void)drawSelectionInRect:(NSRect)dirtyRect {}
- (BOOL)isEmphasized { return NO; }
- (void)setEmphasized:(BOOL)emphasized { [super setEmphasized:NO]; }
- (void)setSelected:(BOOL)selected { [super setSelected:selected]; [self setNeedsDisplay:YES]; }
@end
