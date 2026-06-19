// Nút bevel platinum (target/action như NSButton).
#import <Cocoa/Cocoa.h>

@interface OS9BevelButton : NSControl
@property(nonatomic, copy) NSString *title;
@property(nonatomic, strong) NSImage *icon; // nếu set -> vẽ icon thay cho title
@property(nonatomic) BOOL dropdown;      // vẽ mũi tên ▾ kiểu dropdown (env...)
@property(nonatomic) BOOL isDefault;     // nút mặc định (viền đậm) — Send
@property(nonatomic) BOOL selected;      // trạng thái "đang chọn" (vd tab): vẽ lõm như đang nhấn
@property(nonatomic) BOOL enabledState;
- (instancetype)initWithTitle:(NSString *)title target:(id)target action:(SEL)action;
@end
