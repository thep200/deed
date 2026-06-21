// OS9-style popup: bevel + selected title + ▼ arrow; click to open the menu.
#import <Cocoa/Cocoa.h>

@interface OS9PopupButton : NSControl
@property(nonatomic, strong) NSArray<NSString *> *itemTitles;
@property(nonatomic) NSInteger selectedIndex;
@property(nonatomic, readonly) NSString *selectedTitle;
// If set: clicking calls this block INSTEAD of auto-opening the menu (owner decides when to openMenu).
@property(nonatomic, copy) void (^onClick)(void);
- (instancetype)initWithItems:(NSArray<NSString *> *)items target:(id)target action:(SEL)action;
- (void)selectTitle:(NSString *)title;
- (void)openMenu;   // open current item list (use after async RPC load completes)
@end
