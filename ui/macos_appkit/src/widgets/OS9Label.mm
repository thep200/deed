#import "widgets/OS9Label.h"
#import "theme/OS9Theme.h"

NSTextField *OS9Label(NSString *text) {
    NSTextField *l = [NSTextField labelWithString:text ?: @""];
    l.font = [OS9Theme uiFont];
    l.textColor = [NSColor blackColor];
    l.backgroundColor = [NSColor clearColor];
    l.drawsBackground = NO;
    return l;
}

// Cell tự căn giữa text theo chiều dọc trong bounds.
@interface OS9VCenterCell : NSTextFieldCell
@end
@implementation OS9VCenterCell
- (NSRect)titleRectForBounds:(NSRect)rect {
    NSRect tr = [super titleRectForBounds:rect];
    CGFloat th = [self.attributedStringValue size].height;
    if (th > 0 && th < rect.size.height) {
        tr.origin.y = rect.origin.y + (rect.size.height - th) / 2.0;
        tr.size.height = th;
    }
    return tr;
}
- (void)drawInteriorWithFrame:(NSRect)frame inView:(NSView *)v {
    [super drawInteriorWithFrame:[self titleRectForBounds:frame] inView:v];
}
@end

NSTextField *OS9CenteredLabel(NSString *text) {
    NSTextField *l = [[NSTextField alloc] initWithFrame:NSZeroRect];
    OS9VCenterCell *cell = [[OS9VCenterCell alloc] initTextCell:text ?: @""];
    cell.bezeled = NO; cell.editable = NO; cell.selectable = NO;
    l.cell = cell;
    l.font = [OS9Theme uiFont];
    l.textColor = [NSColor blackColor];
    l.backgroundColor = [NSColor clearColor];
    l.drawsBackground = NO;
    return l;
}
