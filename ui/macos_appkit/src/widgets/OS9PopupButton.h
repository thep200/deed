// Popup kiểu OS9: bevel + tiêu đề đang chọn + mũi ▼; bấm để bung menu.
#import <Cocoa/Cocoa.h>

@interface OS9PopupButton : NSControl
@property(nonatomic, strong) NSArray<NSString *> *itemTitles;
@property(nonatomic) NSInteger selectedIndex;
@property(nonatomic, readonly) NSString *selectedTitle;
- (instancetype)initWithItems:(NSArray<NSString *> *)items target:(id)target action:(SEL)action;
- (void)selectTitle:(NSString *)title;
@end
