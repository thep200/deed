// Toast retro (theo assets/toast.png): viền đen dày + bóng đổ cứng, nền theo loại:
// xám=info, xanh=success, đỏ=fail. Có icon trạng thái bên trái + nút ✕ bên phải.
#import <Cocoa/Cocoa.h>

@interface OS9Toast : NSView
@property(nonatomic) NSInteger kind;            // 0=info(xám) 1=success(xanh) 2=fail(đỏ)
@property(nonatomic, copy) NSString *message;
@property(nonatomic, copy) void (^onClose)(void);  // bấm toast/✕ -> đóng
- (instancetype)initWithMessage:(NSString *)msg kind:(NSInteger)kind;
+ (NSSize)sizeForMessage:(NSString *)msg;       // kích thước (đã gồm bóng đổ)
@end
