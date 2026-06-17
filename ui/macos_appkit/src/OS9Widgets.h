// OS9Widgets — widget tự vẽ kiểu Mac OS 9 (README §5: tách "vỏ" tự vẽ khỏi input native).
#import <Cocoa/Cocoa.h>

// Nút bevel platinum (target/action như NSButton).
@interface OS9BevelButton : NSControl
@property(nonatomic, copy) NSString *title;
@property(nonatomic, strong) NSImage *icon; // nếu set -> vẽ icon thay cho title
@property(nonatomic) BOOL dropdown;      // vẽ mũi tên ▾ kiểu dropdown (env...)
@property(nonatomic) BOOL isDefault;     // nút mặc định (viền đậm) — Send
@property(nonatomic) BOOL enabledState;
- (instancetype)initWithTitle:(NSString *)title target:(id)target action:(SEL)action;
@end

// Vẽ icon bánh răng cổ điển (cog) cỡ size — dùng cho nút Setting.
NSImage *OS9GearImage(CGFloat size);

// Icon "send" (máy bay giấy hướng phải) — thay label nút Send.
NSImage *OS9SendImage(CGFloat size);
// Icon loading (spinner nan hoa) tại pha quay phase∈[0,1) — animate bằng timer.
NSImage *OS9SpinnerImage(CGFloat size, CGFloat phase);

// Popup kiểu OS9: bevel + tiêu đề đang chọn + mũi ▼; bấm để bung menu.
@interface OS9PopupButton : NSControl
@property(nonatomic, strong) NSArray<NSString *> *itemTitles;
@property(nonatomic) NSInteger selectedIndex;
@property(nonatomic, readonly) NSString *selectedTitle;
- (instancetype)initWithItems:(NSArray<NSString *> *)items target:(id)target action:(SEL)action;
- (void)selectTitle:(NSString *)title;
@end

// Cửa sổ kiểu OS9: borderless -> góc VUÔNG (không bo tròn như titled window),
// nhưng vẫn nhận key/main + kéo/resize. Dùng khi muốn look retro vuông vức.
@interface OS9Window : NSWindow
@end

// Title bar kẻ sọc + nút close, kéo cửa sổ được.
@interface OS9TitleBar : NSView
@property(nonatomic, copy) NSString *title;
@property(nonatomic, weak) id closeTarget;
@property(nonatomic) SEL closeAction;
@property(nonatomic, weak) id zoomTarget;     // nil -> performZoom mặc định
@property(nonatomic) SEL zoomAction;
@property(nonatomic, weak) id collapseTarget; // nil -> miniaturize mặc định
@property(nonatomic) SEL collapseAction;
@end

// Nền platinum (tô xám đặc cho content).
@interface OS9BackgroundView : NSView
@end

// Thanh kéo chia pane (dọc): kéo ngang -> báo dx (điểm ảnh) cho handler.
@interface OS9Divider : NSView
@property(nonatomic, copy) void (^onDrag)(CGFloat dx);
@end

// Scrollbar OS9 tự vẽ (theo scrollbar.svg): track #AAAAAA, thumb #9999FF có gân, nút mũi tên.
@interface OS9Scroller : NSScroller
@end

// Khung inset (sunken) — bọc NSScrollView/NSTextField cho ra look OS9.
@interface OS9InsetView : NSView
@end

// Ô input retro: nền trắng + viền răng cưa nhỏ ở các góc (URL / status line).
@interface OS9SerratedInset : NSView
@end

// Toast retro (theo assets/toast.png): viền đen dày + bóng đổ cứng, nền theo loại:
// xám=info, xanh=success, đỏ=fail. Có icon trạng thái bên trái + nút ✕ bên phải.
@interface OS9Toast : NSView
@property(nonatomic) NSInteger kind;            // 0=info(xám) 1=success(xanh) 2=fail(đỏ)
@property(nonatomic, copy) NSString *message;
@property(nonatomic, copy) void (^onClose)(void);  // bấm toast/✕ -> đóng
- (instancetype)initWithMessage:(NSString *)msg kind:(NSInteger)kind;
+ (NSSize)sizeForMessage:(NSString *)msg;       // kích thước (đã gồm bóng đổ)
@end

// Nhãn nhỏ kiểu OS9 (NSTextField không viền).
NSTextField *OS9Label(NSString *text);

// Như OS9Label nhưng CĂN GIỮA theo chiều dọc (dùng cho status line cao hơn dòng chữ).
NSTextField *OS9CenteredLabel(NSString *text);

// Style menu kiểu OS9: mỗi item là 1 view tự vẽ (nền platinum, chọn -> highlight xanh tím).
void OS9StyleMenu(NSMenu *menu);

// Dropdown tuỳ biến: danh sách item GÓC VUÔNG (không dùng NSMenu hệ thống), hiện ngay
// dưới (hoặc trên nếu hết chỗ) anchor. onPick(index) khi chọn; tự đóng khi click ngoài/Esc.
void OS9ShowDropdown(NSArray<NSString *> *items, NSInteger selected, NSView *anchor,
                     void (^onPick)(NSInteger index));
