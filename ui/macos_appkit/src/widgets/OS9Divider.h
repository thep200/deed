// Thanh kéo chia pane (dọc): kéo ngang -> báo dx (điểm ảnh) cho handler.
#import <Cocoa/Cocoa.h>

@interface OS9Divider : NSView
@property(nonatomic, copy) void (^onDrag)(CGFloat dx);
@end
