// Platinum bevel button (target/action like NSButton).
#import <Cocoa/Cocoa.h>

@interface OS9BevelButton : NSControl
@property(nonatomic, copy) NSString *title;
@property(nonatomic, strong) NSImage *icon; // if set -> draw icon instead of title
@property(nonatomic) BOOL dropdown;      // draw ▾ dropdown arrow (env...)
@property(nonatomic) BOOL isDefault;     // default button (bold border) — Send
@property(nonatomic) BOOL selected;      // "selected" state (e.g. tab): draw sunken as if pressed
@property(nonatomic) BOOL enabledState;
- (instancetype)initWithTitle:(NSString *)title target:(id)target action:(SEL)action;
@end
