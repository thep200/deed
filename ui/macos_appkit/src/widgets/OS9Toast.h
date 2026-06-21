// FLAT retro toast: background always gray, only the dashed border is colored by kind
// (colors from assets/color.png): info=blue-gray, success=green, fail=red.
// Has a status icon on the left + ✕ button on the right.
#import <Cocoa/Cocoa.h>

@interface OS9Toast : NSView
@property(nonatomic) NSInteger kind;            // 0=info(gray) 1=success(green) 2=fail(red)
@property(nonatomic, copy) NSString *message;
@property(nonatomic, copy) void (^onClose)(void);  // click toast/✕ -> close
- (instancetype)initWithMessage:(NSString *)msg kind:(NSInteger)kind;
+ (NSSize)sizeForMessage:(NSString *)msg;       // toast size
@end
