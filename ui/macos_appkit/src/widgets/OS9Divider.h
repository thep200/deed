// Vertical pane splitter handle: horizontal drag -> reports dx (pixels) to handler.
#import <Cocoa/Cocoa.h>

@interface OS9Divider : NSView
@property(nonatomic, copy) void (^onDrag)(CGFloat dx);
@end
