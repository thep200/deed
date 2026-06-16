// OS9Theme — bảng màu Platinum (Mac OS 9) + helper vẽ bevel/inset (README §5).
// Look custom, KHÔNG dùng control AppKit mặc định (render Aqua hiện đại).
#import <Cocoa/Cocoa.h>

@interface OS9Theme : NSObject
+ (NSColor *)face;        // xám nền platinum
+ (NSColor *)buttonFace;  // #DDDDDD — nền nút (theo button.svg)
+ (NSColor *)faceLight;   // sáng hơn (gradient nhẹ)
+ (NSColor *)highlight;   // trắng (cạnh trên-trái)
+ (NSColor *)shadow;      // xám đậm (cạnh dưới-phải)
+ (NSColor *)darkShadow;  // đậm hơn
+ (NSColor *)frame;       // viền đen
+ (NSColor *)titleActive; // nền title bar active
+ (NSColor *)windowBg;    // nền cửa sổ
+ (NSColor *)accent;      // xanh chọn (selection)
+ (NSFont  *)uiFont;
+ (NSFont  *)monoFont;
// Cấu hình font từ Settings (rỗng -> mặc định). size<=0 -> 11.
+ (void)setConfiguredFontName:(NSString *)name size:(CGFloat)size;

// Vẽ nút bevel CŨ (button.svg): góc răng cưa, #DDDDDD, viền đen. pressed = lõm.
+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Vẽ nút MỚI (btn-new.svg): góc vuông, #CCCCCC, viền #484848, bevel trắng/xám.
+ (void)drawNewBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Bộ chọn kiểu nút (dùng cho mọi button). Mặc định kiểu MỚI; đổi qua .env BUTTON_STYLE=classic.
+ (void)setClassicButtonStyle:(BOOL)classic;
+ (void)drawButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Vẽ viền inset (sunken) cho ô nhập: tối trên-trái, sáng dưới-phải.
+ (void)drawInsetInRect:(NSRect)r;
// Vẽ title bar kẻ sọc platinum.
+ (void)drawStripedTitleInRect:(NSRect)r active:(BOOL)active;

// Path nút góc bo kiểu pixel (theo button.svg).
+ (NSBezierPath *)steppedPathInRect:(NSRect)r;

// Path góc răng cưa nhỏ (cho ô input URL / status line).
+ (NSBezierPath *)serratedPathInRect:(NSRect)r;

// Ô điều khiển cửa sổ kiểu Mac (theo *_box.svg). glyph: 0=close, 1=collapse, 2=zoom.
+ (void)drawMacControlBox:(NSRect)r glyph:(int)glyph;

// Mũi tên ▾ + vạch ngăn cho dropdown (method/env) — theo dropdown.svg.
+ (void)drawDropdownArrowInRect:(NSRect)r;
@end
