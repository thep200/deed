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
// Bỏ phần thụt mặc định dành cho tam giác disclosure (ta tự vẽ trong cell) -> nội dung sát
// mép trái, GIỮ thụt theo cấp (indentationPerLevel) để con vẫn lùi vào dưới cha.
- (NSRect)frameOfCellAtColumn:(NSInteger)column row:(NSInteger)row {
    NSRect f = [super frameOfCellAtColumn:column row:row];
    CGFloat want = self.indentationPerLevel * [self levelForRow:row];
    CGFloat delta = f.origin.x - want;
    if (delta > 0) { f.origin.x -= delta; f.size.width += delta; }
    return f;
}
@end

// --- Geometry hàng (khớp ảnh OS9 Platinum, SPEC §2) ---
static const CGFloat kTreeLeftMargin = 5;   // lề trái cho tam giác disclosure (cách mép trong panel)
static const CGFloat kTreeGutterW   = 19;   // ô tam giác disclosure (trước icon folder) = margin + tam giác
static const CGFloat kTreeIconW     = 16;   // icon folder
static const CGFloat kTreeIconGap   = 5;    // khoảng icon -> nhãn
static const CGFloat kTreeFileTextPad = 5;  // request: text "method name" lề trái (khớp tam giác)

// Cắt nhãn cho vừa maxW + thêm "…" (drawAtPoint không tự truncate).
static NSString *TreeEllipsize(NSString *s, CGFloat maxW, NSDictionary *attrs) {
    if (maxW <= 0) return @"";
    if ([s sizeWithAttributes:attrs].width <= maxW) return s;
    NSString *e = @"…";
    NSUInteger lo = 0, hi = s.length;
    while (hi > lo) {
        NSUInteger mid = (lo + hi + 1) / 2;
        NSString *cand = [[s substringToIndex:mid] stringByAppendingString:e];
        if ([cand sizeWithAttributes:attrs].width <= maxW) lo = mid; else hi = mid - 1;
    }
    return [[s substringToIndex:lo] stringByAppendingString:e];
}

@implementation TreeCellView
- (BOOL)isFlipped { return YES; }   // toạ độ từ trên xuống (y nhỏ = trên)

- (void)setIsExpanded:(BOOL)e { _isExpanded = e; [self setNeedsDisplay:YES]; }

// Đường biên folder (tab trên-trái + thân) tại góc trên-trái (x,y), y hướng xuống.
static NSBezierPath *TreeFolderPath(CGFloat x, CGFloat y) {
    const CGFloat bodyW = 13, bodyH = 10, tabW = 5, tabH = 2;
    NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(x,             y)];               // tab trên-trái
    [p lineToPoint:NSMakePoint(x + tabW,      y)];               // tab trên-phải
    [p lineToPoint:NSMakePoint(x + tabW + 2,  y + tabH)];        // dốc xuống mép thân
    [p lineToPoint:NSMakePoint(x + bodyW,     y + tabH)];        // mép trên thân (phải)
    [p lineToPoint:NSMakePoint(x + bodyW,     y + tabH + bodyH)];// mép phải
    [p lineToPoint:NSMakePoint(x,             y + tabH + bodyH)];// mép dưới
    [p closePath];                                               // mép trái thẳng (x)
    return p;
}

// Icon folder 3D: mặt trước sáng nổi lên, mặt bên tối lệch xuống-phải (lộ 2 mép) + đổ bóng.
- (void)drawFolderIconInRect:(NSRect)r {
    CGFloat x = r.origin.x + 0.5, y = r.origin.y + 1.5;
    NSColor *face = [NSColor colorWithCalibratedWhite:0.88 alpha:1.0];   // mặt trước (sáng)
    NSColor *side = [NSColor colorWithCalibratedWhite:0.56 alpha:1.0];   // mặt bên/depth (tối)
    NSColor *line = [OS9Theme frame];
    const CGFloat d = 2.5;   // độ dày 3D (lệch chéo trái->phải, xuống dưới)

    // 1) đổ bóng mờ (xa nhất, dưới-phải)
    [[NSColor colorWithCalibratedWhite:0.40 alpha:0.30] set];
    [TreeFolderPath(x + d + 1.0, y + d + 1.0) fill];

    // 2) mặt bên (tối) lệch xuống-phải -> lộ độ dày 3D ở mép phải + đáy
    NSBezierPath *back = TreeFolderPath(x + d, y + d);
    [side set]; [back fill];
    [line set]; back.lineWidth = 1.0; [back stroke];

    // 3) mặt trước (sáng) ở gốc -> nổi lên, thấy rõ 2 mép (mặt trước + mặt bên)
    NSBezierPath *front = TreeFolderPath(x, y);
    [face set]; [front fill];
    [[NSColor colorWithCalibratedWhite:0.97 alpha:1.0] set];   // bevel sáng mép trên thân
    NSRectFill(NSMakeRect(x + 1, y + 3, 11, 1));
    [line set]; front.lineWidth = 1.0; [front stroke];
}

- (void)drawRect:(NSRect)d {
    CGFloat h = self.bounds.size.height;
    CGFloat cy = floor(h / 2.0);
    CGFloat tx = 0;

    if (_isFolder) {
        [NSGraphicsContext saveGraphicsState];
        [[NSGraphicsContext currentContext] setShouldAntialias:NO];   // nét bitmap sắc kiểu OS9
        // Tam giác disclosure pixel: hẹp & CAO (cao > rộng), viền trái ĐẬM, 2 cạnh chéo răng cưa.
        [[OS9Theme frame] set];
        const CGFloat gx = kTreeLeftMargin;   // lề trái cho tam giác (đã bỏ thụt disclosure mặc định)
        if (_isExpanded) {
            // ▽ trỏ xuống: rộng > cao; cạnh TRÊN đậm, 2 cạnh chéo răng cưa.
            const CGFloat w = 9, hgt = 6;
            CGFloat ex = gx, ey = cy - floor(hgt / 2);
            NSRectFill(NSMakeRect(ex, ey, w, 1.5));               // cạnh trên đậm
            NSBezierPath *e = [NSBezierPath bezierPath];
            [e moveToPoint:NSMakePoint(ex,         ey)];
            [e lineToPoint:NSMakePoint(ex + w / 2, ey + hgt)];    // chéo trái->đáy
            [e moveToPoint:NSMakePoint(ex + w,     ey)];
            [e lineToPoint:NSMakePoint(ex + w / 2, ey + hgt)];    // chéo phải->đáy
            e.lineWidth = 1.0; [e stroke];                         // AA off -> răng cưa pixel
        } else {
            // ▷ trỏ phải: cao > rộng (dẹp); viền TRÁI đậm, 2 cạnh chéo răng cưa.
            const CGFloat w = 5, hgt = 11;
            CGFloat ax = gx, ay = cy - floor(hgt / 2);
            NSRectFill(NSMakeRect(ax, ay, 2, hgt));               // viền trái đậm (bar 2px)
            NSBezierPath *e = [NSBezierPath bezierPath];
            [e moveToPoint:NSMakePoint(ax,     ay)];
            [e lineToPoint:NSMakePoint(ax + w, cy)];              // chéo trên->đỉnh phải
            [e moveToPoint:NSMakePoint(ax,     ay + hgt)];
            [e lineToPoint:NSMakePoint(ax + w, cy)];              // chéo dưới->đỉnh phải
            e.lineWidth = 1.0; [e stroke];                         // AA off -> răng cưa pixel
        }
        // Icon folder OS9 tại gutter
        [self drawFolderIconInRect:NSMakeRect(kTreeGutterW, cy - 8, kTreeIconW, 16)];
        [NSGraphicsContext restoreGraphicsState];
        tx = kTreeGutterW + kTreeIconW + kTreeIconGap;   // nhãn folder sau icon
    }

    NSDictionary *attrs = @{ NSFontAttributeName : [OS9Theme uiFont],
                             NSForegroundColorAttributeName : [NSColor blackColor] };
    CGFloat ty = floor((h - [@"Mg" sizeWithAttributes:attrs].height) / 2);

    if (_isFolder) {
        // Nhãn folder: ellipsize cuối + tooltip khi bị cắt.
        CGFloat avail = self.bounds.size.width - tx - 4;
        NSString *full = _text ?: @"";
        NSString *show = TreeEllipsize(full, avail, attrs);
        self.toolTip = [show isEqualToString:full] ? nil : full;
        [show drawAtPoint:NSMakePoint(tx, ty) withAttributes:attrs];
    } else {
        // Request: method CĂN PHẢI trong cột cố định -> ký tự cuối thẳng hàng (vd "GET"/"POST"
        // thì 2 chữ T cùng mốc). Tên bắt đầu sau cột (mốc x cố định) -> tên cũng thẳng hàng.
        CGFloat pad = kTreeFileTextPad;
        CGFloat methodColW = ceil([@"OPTIONS" sizeWithAttributes:attrs].width);
        CGFloat methodRight = pad + methodColW;   // mép phải cột method (mọi method canh tới đây)
        NSString *mk = _mark ?: @"";
        CGFloat mkW = [mk sizeWithAttributes:attrs].width;
        [mk drawAtPoint:NSMakePoint(methodRight - mkW, ty) withAttributes:attrs];   // canh phải
        CGFloat nameX = methodRight + 8;          // khoảng cách cố định method -> tên
        CGFloat avail = self.bounds.size.width - nameX - 4;
        NSString *full = _text ?: @"";
        NSString *show = TreeEllipsize(full, avail, attrs);
        self.toolTip = [show isEqualToString:full] ? nil : full;
        [show drawAtPoint:NSMakePoint(nameX, ty) withAttributes:attrs];
    }
}
@end

@implementation OS9RowView
// Tô nền xám nhẹ khi row được chọn (đơn HOẶC nhiều); KHÔNG đảo chữ trắng.
- (void)drawBackgroundInRect:(NSRect)dirtyRect {
    [[NSColor whiteColor] set];
    NSRectFill(self.bounds);
    if (self.selected) { [[OS9Theme rowSelectionGray] set]; NSRectFill(self.bounds); }
    // Đường kẻ ngăn hàng 1px full-width (Platinum nhạt) — idempotent, antialias off.
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [[NSColor colorWithCalibratedWhite:0.84 alpha:1.0] set];
    NSRect b = self.bounds;
    NSRectFill(NSMakeRect(0, 0, b.size.width, 1));   // đáy hàng (non-flipped: y=0 = đáy)
    [NSGraphicsContext restoreGraphicsState];
}
// Vô hiệu mọi hiệu ứng selection/emphasized mặc định (không vẽ xanh ở bất kỳ trạng thái nào).
- (void)drawSelectionInRect:(NSRect)dirtyRect {}
- (BOOL)isEmphasized { return NO; }
- (void)setEmphasized:(BOOL)emphasized { [super setEmphasized:NO]; }
- (void)setSelected:(BOOL)selected { [super setSelected:selected]; [self setNeedsDisplay:YES]; }
@end
