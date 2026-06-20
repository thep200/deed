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
+ (NSColor *)rowSelectionGray; // nền xám nhẹ cho row được chọn trong cây (thay xanh mặc định)
+ (NSFont  *)uiFont;
+ (NSFont  *)monoFont;
// Font cấu hình ở size/đậm tuỳ vai trò (heading, title...) NHƯNG cùng họ chữ với uiFont.
+ (NSFont  *)uiFontOfSize:(CGFloat)size bold:(BOOL)bold;
// Font cấu hình (đúng size người dùng đặt) + đậm — cho title bar & tiêu đề màn (KHÔNG hardcode size).
+ (NSFont  *)boldUiFont;
// Tên họ chữ đã cấu hình (nil nếu dùng mặc định) + size — cho engine ngoài AppKit (Scintilla).
+ (NSString *)configuredFontName;
+ (CGFloat)configuredFontSize;
// Cấu hình font từ Settings (rỗng -> mặc định). size<=0 -> 11.
+ (void)setConfiguredFontName:(NSString *)name size:(CGFloat)size;

// Vẽ nút bevel CŨ (button.svg): góc răng cưa, #DDDDDD, viền đen. pressed = lõm.
+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Vẽ nút MỚI (btn-new.svg): góc vuông, #CCCCCC, viền #484848, bevel trắng/xám.
+ (void)drawNewBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Vẽ nút kiểu LINE (retro, border-line): nền phẳng + viền nét, góc vuông; nhấn = đảo nền.
+ (void)drawLineButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Bộ chọn kiểu nút (dùng cho mọi button): "line" (mặc định) | "new" | "classic" (.env BUTTON_STYLE).
+ (void)setButtonStyleName:(NSString *)name;
+ (void)setClassicButtonStyle:(BOOL)classic;   // giữ tương thích (classic<->new)
+ (void)drawButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Màu chữ phù hợp trạng thái nút (kiểu line khi nhấn đảo nền -> chữ trắng).
+ (NSColor *)buttonFGPressed:(BOOL)pressed enabled:(BOOL)enabled;
// Vẽ viền inset (sunken) cho ô nhập: tối trên-trái, sáng dưới-phải.
+ (void)drawInsetInRect:(NSRect)r;
// Vẽ title bar kẻ sọc platinum. Sọc chỉ vẽ trong stripesRect (giữa 2 cụm icon),
// nền nền (band) vẫn phủ toàn bộ r.
+ (void)drawStripedTitleInRect:(NSRect)r stripesInRect:(NSRect)stripesRect active:(BOOL)active;

// Path nút góc bo kiểu pixel (theo button.svg).
+ (NSBezierPath *)steppedPathInRect:(NSRect)r;

// Path góc răng cưa nhỏ (cho ô input URL / status line).
+ (NSBezierPath *)serratedPathInRect:(NSRect)r;

// Ô điều khiển cửa sổ kiểu Mac (theo *_box.svg). glyph: 0=close, 1=collapse, 2=zoom.
+ (void)drawMacControlBox:(NSRect)r glyph:(int)glyph;

// === Title bar OS9 Platinum (PROMPT_os9_titlebar_objcpp.md — vẽ pixel-accurate) ===
// Nền thanh active: viền dưới #262626 + nền #CCCCCC.
+ (void)drawTitleBarFrameInRect:(NSRect)r;
// Nền thanh INACTIVE: phẳng #D6D6D6, không pinstripe (nút + icon do title bar vẽ).
+ (void)drawTitleBarInactiveInRect:(NSRect)r;
// Dải vân pinstripe: nền #DDDDDD, cạnh #EEEEEE/#C5C5C5, đường #999999 mỗi 2px;
// cao cố định 13px căn giữa dọc trong r, co giãn theo r.width.
+ (void)drawTitleGripInRect:(NSRect)r;
// Nút title (close/zoom/collapse) — hộp bevel hình vuông cạnh r.size.width.
// Cấu trúc (ngoài→trong, mỗi lớp 1px): outer bevel (TL #808080 / BR #FFFFFF, lõm) ->
// khung đen #262626 -> mặt gradient dọc #C9C9C9(đỉnh)→#F1F1F1(đáy) -> inner bevel
// (TL #FFFFFF / BR #9A9A9A, nổi) -> glyph #262626.
// glyph: 0=close (trống), 1=zoom (ô vuông nhỏ góc trên-trái dùng chung cạnh),
//        2=collapse (2 vạch ngang chạm 2 cạnh -> 3 dải, windowshade).
// pressed=YES -> phủ overlay #353535→#9C9C9C @0.8 chéo TL→BR lên mặt (mouse-down).
+ (void)drawTitleButtonInRect:(NSRect)r glyph:(int)glyph pressed:(BOOL)pressed;

// Mũi tên ▾ + vạch ngăn cho dropdown (method/env) — theo dropdown.svg.
+ (void)drawDropdownArrowInRect:(NSRect)r;
@end
