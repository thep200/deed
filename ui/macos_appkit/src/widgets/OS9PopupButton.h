// Popup kiểu OS9: bevel + tiêu đề đang chọn + mũi ▼; bấm để bung menu.
#import <Cocoa/Cocoa.h>

@interface OS9PopupButton : NSControl
@property(nonatomic, strong) NSArray<NSString *> *itemTitles;
@property(nonatomic) NSInteger selectedIndex;
@property(nonatomic, readonly) NSString *selectedTitle;
// Nếu set: bấm nút sẽ gọi block này THAY VÌ tự bung menu (owner tự quyết khi nào openMenu).
@property(nonatomic, copy) void (^onClick)(void);
- (instancetype)initWithItems:(NSArray<NSString *> *)items target:(id)target action:(SEL)action;
- (void)selectTitle:(NSString *)title;
- (void)openMenu;   // bung danh sách item hiện tại (dùng sau khi nạp xong RPC bất đồng bộ)
@end
