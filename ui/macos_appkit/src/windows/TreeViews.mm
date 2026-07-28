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
        it.requestType = n.requestType;
        it.grpc = (n.requestType == core::RequestType::Grpc);
        it.badge = [NSString stringWithFormat:@"%s %s", core::toString(n.requestType).c_str(), n.methodOrType.c_str()];
        // HTTP -> method name (GET/POST...); gRPC -> "gRPC"; WebSocket -> "WS"; GraphQL -> "GQL";
        // Kafka -> "KAFKA"; SOAP -> "SOAP" (methodOrType is empty for method-less types).
        it.mark = it.grpc ? @"gRPC"
                : (n.requestType == core::RequestType::WebSocket) ? @"WS"
                : (n.requestType == core::RequestType::GraphQl) ? @"GQL"
                : (n.requestType == core::RequestType::Kafka) ? @"KAFKA"
                : (n.requestType == core::RequestType::Soap) ? @"SOAP"
                : N(n.methodOrType);
    }
    return it;
}

@implementation DeedOutlineView
- (NSMenu *)menuForEvent:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    if (self.contextHandler) self.contextHandler(row, e.locationInWindow);
    return nil;   // we draw our own retro overlay menu, not the system NSMenu
}
// Drop the fold arrow (disclosure triangle) — we draw our own in the cell.
- (NSRect)frameOfOutlineCellAtRow:(NSInteger)row { return NSZeroRect; }
// Drop the default indent reserved for the disclosure triangle (we draw it in the cell) -> content
// hugs the left edge, but KEEP per-level indent (indentationPerLevel) so children stay under parent.
- (NSRect)frameOfCellAtColumn:(NSInteger)column row:(NSInteger)row {
    NSRect f = [super frameOfCellAtColumn:column row:row];
    CGFloat want = self.indentationPerLevel * [self levelForRow:row];
    CGFloat delta = f.origin.x - want;
    if (delta > 0) { f.origin.x -= delta; f.size.width += delta; }
    return f;
}
@end

// --- Row geometry (matches OS9 Platinum reference, SPEC §2) ---
static const CGFloat kTreeLeftMargin = 5;   // left margin for disclosure triangle (from panel inner edge)
static const CGFloat kTreeGutterW   = 19;   // disclosure-triangle cell (before folder icon) = margin + triangle
static const CGFloat kTreeIconW     = 16;   // folder icon
static const CGFloat kTreeIconGap   = 5;    // gap icon -> label
static const CGFloat kTreeFileTextPad = 5;  // request: left padding of "method name" text (aligns with triangle)

// Trim a label to fit maxW + append "…" (drawAtPoint does not truncate).
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

// --- Pixel-art disclosure arrows (source: assets/Position={Down,Right}…Purple.svg) ---
// Each pixel = half a point (SVG 8x8pt, 0.5 grid) -> crisp 1:1 on Retina. Converted the
// purple gradient to gray (R==G in every purple, so use the R channel as gray level); #262626 border kept.
typedef struct { unsigned char x, y, gray; float a; } TreePx;

// ▽ pointing down (folder open) — 16x10 half-pt = 8x5 pt.
static const TreePx kTreeArrowDown[] = {
    {0,0,38,0.05f}, {1,0,38,0.20f}, {2,0,38,0.40f}, {3,0,38,0.40f}, {4,0,38,0.40f},
    {5,0,38,0.40f}, {6,0,38,0.40f}, {7,0,38,0.40f}, {8,0,38,0.40f}, {9,0,38,0.40f},
    {10,0,38,0.40f}, {11,0,38,0.40f}, {12,0,38,0.40f}, {13,0,38,0.40f}, {14,0,38,0.20f},
    {15,0,38,0.05f}, {0,1,38,0.20f}, {1,1,38,0.70f}, {2,1,38,1.00f}, {3,1,38,1.00f},
    {4,1,38,1.00f}, {5,1,38,1.00f}, {6,1,38,1.00f}, {7,1,38,1.00f}, {8,1,38,1.00f},
    {9,1,38,1.00f}, {10,1,38,1.00f}, {11,1,38,1.00f}, {12,1,38,1.00f}, {13,1,38,1.00f},
    {14,1,38,0.70f}, {15,1,38,0.20f}, {0,2,38,0.20f}, {1,2,38,0.70f}, {2,2,38,1.00f},
    {3,2,191,1.00f}, {4,2,191,1.00f}, {5,2,191,1.00f}, {6,2,191,1.00f}, {7,2,191,1.00f},
    {8,2,191,1.00f}, {9,2,191,1.00f}, {10,2,191,1.00f}, {11,2,191,1.00f}, {12,2,191,1.00f},
    {13,2,38,1.00f}, {14,2,38,0.70f}, {15,2,38,0.20f}, {0,3,38,0.05f}, {1,3,38,0.20f},
    {2,3,38,0.70f}, {3,3,38,1.00f}, {4,3,166,1.00f}, {5,3,166,1.00f}, {6,3,166,1.00f},
    {7,3,166,1.00f}, {8,3,166,1.00f}, {9,3,166,1.00f}, {10,3,166,1.00f}, {11,3,166,1.00f},
    {12,3,38,1.00f}, {13,3,38,0.70f}, {14,3,38,0.20f}, {15,3,38,0.05f}, {2,4,38,0.20f},
    {3,4,38,0.70f}, {4,4,38,1.00f}, {5,4,153,1.00f}, {6,4,153,1.00f}, {7,4,153,1.00f},
    {8,4,153,1.00f}, {9,4,153,1.00f}, {10,4,153,1.00f}, {11,4,38,1.00f}, {12,4,38,0.70f},
    {13,4,38,0.20f}, {2,5,38,0.05f}, {3,5,38,0.20f}, {4,5,38,0.70f}, {5,5,38,1.00f},
    {6,5,134,1.00f}, {7,5,134,1.00f}, {8,5,134,1.00f}, {9,5,134,1.00f}, {10,5,38,1.00f},
    {11,5,38,0.70f}, {12,5,38,0.20f}, {13,5,38,0.05f}, {4,6,38,0.20f}, {5,6,38,0.70f},
    {6,6,38,1.00f}, {7,6,96,1.00f}, {8,6,96,1.00f}, {9,6,38,1.00f}, {10,6,38,0.70f},
    {11,6,38,0.20f}, {4,7,38,0.05f}, {5,7,38,0.20f}, {6,7,38,0.70f}, {7,7,38,1.00f},
    {8,7,38,1.00f}, {9,7,38,0.70f}, {10,7,38,0.20f}, {11,7,38,0.05f}, {6,8,38,0.20f},
    {7,8,38,0.70f}, {8,8,38,0.70f}, {9,8,38,0.20f}, {6,9,38,0.05f}, {7,9,38,0.20f},
    {8,9,38,0.20f}, {9,9,38,0.05f},
};

// ▷ pointing right (folder closed) — 10x16 half-pt = 5x8 pt.
static const TreePx kTreeArrowRight[] = {
    {0,0,38,0.05f}, {1,0,38,0.20f}, {2,0,38,0.20f}, {3,0,38,0.05f}, {0,1,38,0.20f},
    {1,1,38,0.70f}, {2,1,38,0.70f}, {3,1,38,0.20f}, {0,2,38,0.40f}, {1,2,38,1.00f},
    {2,2,38,1.00f}, {3,2,38,0.70f}, {4,2,38,0.20f}, {5,2,38,0.05f}, {0,3,38,0.40f},
    {1,3,38,1.00f}, {2,3,191,1.00f}, {3,3,38,1.00f}, {4,3,38,0.70f}, {5,3,38,0.20f},
    {0,4,38,0.40f}, {1,4,38,1.00f}, {2,4,191,1.00f}, {3,4,166,1.00f}, {4,4,38,1.00f},
    {5,4,38,0.70f}, {6,4,38,0.20f}, {7,4,38,0.05f}, {0,5,38,0.40f}, {1,5,38,1.00f},
    {2,5,191,1.00f}, {3,5,166,1.00f}, {4,5,153,1.00f}, {5,5,38,1.00f}, {6,5,38,0.70f},
    {7,5,38,0.20f}, {0,6,38,0.40f}, {1,6,38,1.00f}, {2,6,191,1.00f}, {3,6,166,1.00f},
    {4,6,153,1.00f}, {5,6,134,1.00f}, {6,6,38,1.00f}, {7,6,38,0.70f}, {8,6,38,0.20f},
    {9,6,38,0.05f}, {0,7,38,0.40f}, {1,7,38,1.00f}, {2,7,191,1.00f}, {3,7,166,1.00f},
    {4,7,153,1.00f}, {5,7,134,1.00f}, {6,7,96,1.00f}, {7,7,38,1.00f}, {8,7,38,0.70f},
    {9,7,38,0.20f}, {0,8,38,0.40f}, {1,8,38,1.00f}, {2,8,191,1.00f}, {3,8,166,1.00f},
    {4,8,153,1.00f}, {5,8,134,1.00f}, {6,8,96,1.00f}, {7,8,38,1.00f}, {8,8,38,0.70f},
    {9,8,38,0.20f}, {0,9,38,0.40f}, {1,9,38,1.00f}, {2,9,191,1.00f}, {3,9,166,1.00f},
    {4,9,153,1.00f}, {5,9,134,1.00f}, {6,9,38,1.00f}, {7,9,38,0.70f}, {8,9,38,0.20f},
    {9,9,38,0.05f}, {0,10,38,0.40f}, {1,10,38,1.00f}, {2,10,191,1.00f}, {3,10,166,1.00f},
    {4,10,153,1.00f}, {5,10,38,1.00f}, {6,10,38,0.70f}, {7,10,38,0.20f}, {0,11,38,0.40f},
    {1,11,38,1.00f}, {2,11,191,1.00f}, {3,11,166,1.00f}, {4,11,38,1.00f}, {5,11,38,0.70f},
    {6,11,38,0.20f}, {7,11,38,0.05f}, {0,12,38,0.40f}, {1,12,38,1.00f}, {2,12,191,1.00f},
    {3,12,38,1.00f}, {4,12,38,0.70f}, {5,12,38,0.20f}, {0,13,38,0.40f}, {1,13,38,1.00f},
    {2,13,38,1.00f}, {3,13,38,0.70f}, {4,13,38,0.20f}, {5,13,38,0.05f}, {0,14,38,0.20f},
    {1,14,38,0.70f}, {2,14,38,0.70f}, {3,14,38,0.20f}, {0,15,38,0.05f}, {1,15,38,0.20f},
    {2,15,38,0.20f}, {3,15,38,0.05f},
};

// Size of one pixel-art unit (points). 0.5 = exact SVG scale (8x8pt); raise for a bigger arrow.
static const CGFloat kArrowPx = 0.625;   // ~+25% vs original

// Draw pixel-art at top-left corner (ox,oy) in the cell's FLIPPED coords (y downward,
// matching SVG coordinate system). SourceOver blend to keep the faint border (alpha < 1).
static void DrawTreeArrow(const TreePx *px, size_t n, CGFloat ox, CGFloat oy) {
    const CGFloat s = kArrowPx;
    for (size_t i = 0; i < n; i++) {
        [[NSColor colorWithCalibratedWhite:px[i].gray / 255.0 alpha:px[i].a] set];
        NSRectFillUsingOperation(NSMakeRect(ox + px[i].x * s, oy + px[i].y * s, s, s),
                                 NSCompositingOperationSourceOver);
    }
}

@implementation TreeCellView
- (BOOL)isFlipped { return YES; }   // top-down coords (small y = top)

- (void)setIsExpanded:(BOOL)e { _isExpanded = e; [self setNeedsDisplay:YES]; }

// Folder outline (top-left tab + body) at top-left corner (x,y), y downward.
static NSBezierPath *TreeFolderPath(CGFloat x, CGFloat y) {
    const CGFloat bodyW = 13, bodyH = 10, tabW = 5, tabH = 2;
    NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(x,             y)];               // tab top-left
    [p lineToPoint:NSMakePoint(x + tabW,      y)];               // tab top-right
    [p lineToPoint:NSMakePoint(x + tabW + 2,  y + tabH)];        // slope down to body edge
    [p lineToPoint:NSMakePoint(x + bodyW,     y + tabH)];        // body top edge (right)
    [p lineToPoint:NSMakePoint(x + bodyW,     y + tabH + bodyH)];// right edge
    [p lineToPoint:NSMakePoint(x,             y + tabH + bodyH)];// bottom edge
    [p closePath];                                               // straight left edge (x)
    return p;
}

// 3D folder icon: bright front face raised, dark side face offset down-right (shows 2 edges) + shadow.
- (void)drawFolderIconInRect:(NSRect)r {
    CGFloat x = r.origin.x + 0.5, y = r.origin.y + 1.5;
    NSColor *face = [NSColor colorWithCalibratedWhite:0.88 alpha:1.0];   // front face (bright)
    NSColor *side = [NSColor colorWithCalibratedWhite:0.56 alpha:1.0];   // side/depth face (dark)
    NSColor *line = [OS9Theme frame];
    const CGFloat d = 2.5;   // 3D thickness (diagonal offset left->right, downward)

    // 1) faint drop shadow (farthest, bottom-right)
    [[NSColor colorWithCalibratedWhite:0.40 alpha:0.30] set];
    [TreeFolderPath(x + d + 1.0, y + d + 1.0) fill];

    // 2) side face (dark) offset down-right -> reveals 3D thickness at right edge + bottom
    NSBezierPath *back = TreeFolderPath(x + d, y + d);
    [side set]; [back fill];
    [line set]; back.lineWidth = 1.0; [back stroke];

    // 3) front face (bright) at origin -> raised, both edges visible (front + side face)
    NSBezierPath *front = TreeFolderPath(x, y);
    [face set]; [front fill];
    [[NSColor colorWithCalibratedWhite:0.97 alpha:1.0] set];   // bright bevel on body top edge
    NSRectFill(NSMakeRect(x + 1, y + 3, 11, 1));
    [line set]; front.lineWidth = 1.0; [front stroke];
}

- (void)drawRect:(NSRect)d {
    CGFloat h = self.bounds.size.height;
    CGFloat cy = floor(h / 2.0);
    CGFloat tx = 0;

    if (_isFolder) {
        [NSGraphicsContext saveGraphicsState];
        [[NSGraphicsContext currentContext] setShouldAntialias:NO];   // crisp OS9-style bitmap edges
        // Pixel-art disclosure arrow (gray) rebuilt from assets/Position=*.svg.
        const CGFloat arrowCenterX = kTreeLeftMargin + 4.0;   // horizontal center of arrow area
        if (_isExpanded) {
            // ▽ pointing down: 16x10 pixel grid — vertically centered in the row.
            const CGFloat aw = 16 * kArrowPx, ah = 10 * kArrowPx;
            DrawTreeArrow(kTreeArrowDown, sizeof(kTreeArrowDown) / sizeof(TreePx),
                          arrowCenterX - aw / 2, cy - ah / 2);
        } else {
            // ▷ pointing right: 10x16 pixel grid — vertically centered in the row.
            const CGFloat aw = 10 * kArrowPx, ah = 16 * kArrowPx;
            DrawTreeArrow(kTreeArrowRight, sizeof(kTreeArrowRight) / sizeof(TreePx),
                          arrowCenterX - aw / 2, cy - ah / 2);
        }
        // OS9 folder icon in the gutter
        [self drawFolderIconInRect:NSMakeRect(kTreeGutterW, cy - 8, kTreeIconW, 16)];
        [NSGraphicsContext restoreGraphicsState];
        tx = kTreeGutterW + kTreeIconW + kTreeIconGap;   // folder label after icon
    }

    NSDictionary *attrs = @{ NSFontAttributeName : [OS9Theme uiFont],
                             NSForegroundColorAttributeName : [NSColor blackColor] };
    CGFloat ty = floor((h - [@"Mg" sizeWithAttributes:attrs].height) / 2);

    if (_isFolder) {
        // Folder label: trailing ellipsis + tooltip when truncated.
        CGFloat avail = self.bounds.size.width - tx - 4;
        NSString *full = _text ?: @"";
        NSString *show = TreeEllipsize(full, avail, attrs);
        self.toolTip = [show isEqualToString:full] ? nil : full;
        [show drawAtPoint:NSMakePoint(tx, ty) withAttributes:attrs];
    } else {
        // Request: method RIGHT-ALIGNED in a fixed column -> last chars align (e.g. "GET"/"POST"
        // share the final T position). Name starts after the column (fixed x) -> names align too.
        CGFloat pad = kTreeFileTextPad;
        CGFloat methodColW = ceil([@"OPTIONS" sizeWithAttributes:attrs].width);
        CGFloat methodRight = pad + methodColW;   // right edge of method column (all methods align here)
        NSString *mk = _mark ?: @"";
        CGFloat mkW = [mk sizeWithAttributes:attrs].width;
        [mk drawAtPoint:NSMakePoint(methodRight - mkW, ty) withAttributes:attrs];   // right-align
        CGFloat nameX = methodRight + 8;          // fixed gap method -> name
        CGFloat avail = self.bounds.size.width - nameX - 4;
        NSString *full = _text ?: @"";
        NSString *show = TreeEllipsize(full, avail, attrs);
        self.toolTip = [show isEqualToString:full] ? nil : full;
        [show drawAtPoint:NSMakePoint(nameX, ty) withAttributes:attrs];
    }
}
@end

@implementation OS9RowView
// Fill a light gray background when the row is selected (single OR multi); do NOT invert to white text.
- (void)drawBackgroundInRect:(NSRect)dirtyRect {
    [[NSColor whiteColor] set];
    NSRectFill(self.bounds);
    if (self.selected) { [[OS9Theme rowSelectionGray] set]; NSRectFill(self.bounds); }
    // 1px full-width row separator (light Platinum) — idempotent, antialias off.
    [NSGraphicsContext saveGraphicsState];
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    [[NSColor colorWithCalibratedWhite:0.84 alpha:1.0] set];
    NSRect b = self.bounds;
    NSRectFill(NSMakeRect(0, 0, b.size.width, 1));   // row bottom (non-flipped: y=0 = bottom)
    [NSGraphicsContext restoreGraphicsState];
}
// Disable all default selection/emphasized effects (never draw blue in any state).
- (void)drawSelectionInRect:(NSRect)dirtyRect {}
- (BOOL)isEmphasized { return NO; }
- (void)setEmphasized:(BOOL)emphasized { [super setEmphasized:NO]; }
- (void)setSelected:(BOOL)selected { [super setSelected:selected]; [self setNeedsDisplay:YES]; }
@end
