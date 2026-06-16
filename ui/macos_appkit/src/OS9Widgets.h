// OS9Widgets — widget tự vẽ kiểu Mac OS 9 (README §5: tách "vỏ" tự vẽ khỏi input native).
#import <Cocoa/Cocoa.h>

// Nút bevel platinum (target/action như NSButton).
@interface OS9BevelButton : NSControl
@property(nonatomic, copy) NSString *title;
@property(nonatomic) BOOL isDefault;     // nút mặc định (viền đậm) — Send
@property(nonatomic) BOOL enabledState;
- (instancetype)initWithTitle:(NSString *)title target:(id)target action:(SEL)action;
@end

// Popup kiểu OS9: bevel + tiêu đề đang chọn + mũi ▼; bấm để bung menu.
@interface OS9PopupButton : NSControl
@property(nonatomic, strong) NSArray<NSString *> *itemTitles;
@property(nonatomic) NSInteger selectedIndex;
@property(nonatomic, readonly) NSString *selectedTitle;
- (instancetype)initWithItems:(NSArray<NSString *> *)items target:(id)target action:(SEL)action;
- (void)selectTitle:(NSString *)title;
@end

// Title bar kẻ sọc + nút close, kéo cửa sổ được.
@interface OS9TitleBar : NSView
@property(nonatomic, copy) NSString *title;
@property(nonatomic, weak) id closeTarget;
@property(nonatomic) SEL closeAction;
@end

// Nền platinum (tô xám đặc cho content).
@interface OS9BackgroundView : NSView
@end

// Thanh kéo chia pane (dọc): kéo ngang -> báo dx (điểm ảnh) cho handler.
@interface OS9Divider : NSView
@property(nonatomic, copy) void (^onDrag)(CGFloat dx);
@end

// Khung inset (sunken) — bọc NSScrollView/NSTextField cho ra look OS9.
@interface OS9InsetView : NSView
@end

// Nhãn nhỏ kiểu OS9 (NSTextField không viền).
NSTextField *OS9Label(NSString *text);
