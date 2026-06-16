// OS9Theme — bảng màu Platinum (Mac OS 9) + helper vẽ bevel/inset (README §5).
// Look custom, KHÔNG dùng control AppKit mặc định (render Aqua hiện đại).
#import <Cocoa/Cocoa.h>

@interface OS9Theme : NSObject
+ (NSColor *)face;        // xám nền platinum
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

// Vẽ nút bevel: highlight trên-trái, shadow dưới-phải, viền đen. pressed = lõm.
+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Vẽ viền inset (sunken) cho ô nhập: tối trên-trái, sáng dưới-phải.
+ (void)drawInsetInRect:(NSRect)r;
// Vẽ title bar kẻ sọc platinum.
+ (void)drawStripedTitleInRect:(NSRect)r active:(BOOL)active;
@end
