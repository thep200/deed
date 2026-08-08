// Platinum (Mac OS 9) palette + bevel/inset drawing; does NOT use default AppKit controls.
#import <Cocoa/Cocoa.h>

@interface OS9Theme : NSObject
+ (NSColor *)face;        // platinum background gray
+ (NSColor *)buttonFace;  // #DDDDDD — button face (per button.svg)
+ (NSColor *)faceLight;   // lighter (subtle gradient)
+ (NSColor *)highlight;   // white (top-left edge)
+ (NSColor *)shadow;      // dark gray (bottom-right edge)
+ (NSColor *)darkShadow;  // darker
+ (NSColor *)frame;       // black border
+ (NSColor *)titleActive; // active title bar background
+ (NSColor *)windowBg;    // window background
+ (NSColor *)accent;      // selection blue
+ (NSColor *)rowSelectionGray; // subtle gray for selected tree row (instead of default blue)
+ (NSFont  *)uiFont;
+ (NSFont  *)monoFont;
// Configured font at role-specific size/weight (heading, title...) BUT same family as uiFont.
+ (NSFont  *)uiFontOfSize:(CGFloat)size bold:(BOOL)bold;
// Configured font (at the exact user-set size) + bold — for title bar & screen titles (do NOT hardcode size).
+ (NSFont  *)boldUiFont;
// Configured font family name (nil if using default) + size — for non-AppKit engine (Scintilla).
+ (NSString *)configuredFontName;
+ (CGFloat)configuredFontSize;
// SHARED tail-truncating paragraph style (constant) — avoids alloc per drawRect/per row.
+ (NSParagraphStyle *)truncatingTailStyle;
// Configure font from Settings (empty -> default). size<=0 -> 11.
+ (void)setConfiguredFontName:(NSString *)name size:(CGFloat)size;

// Draw OLD bevel button (button.svg): serrated corners, #DDDDDD, black border. pressed = sunken.
+ (void)drawBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Draw NEW button (btn-new.svg): square corners, #CCCCCC, #484848 border, white/gray bevel.
+ (void)drawNewBevelInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Draw LINE-style button (retro, border-line): flat fill + stroked border, square corners; pressed = inverted fill.
+ (void)drawLineButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Button style selector (for all buttons): "line" (default) | "new" | "classic" (.env BUTTON_STYLE).
+ (void)setButtonStyleName:(NSString *)name;
+ (void)setClassicButtonStyle:(BOOL)classic;
+ (void)drawButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Text color matching button state (line style inverts fill when pressed -> white text).
+ (NSColor *)buttonFGPressed:(BOOL)pressed enabled:(BOOL)enabled;
// Draw inset (sunken) border for input field: dark top-left, light bottom-right.
+ (void)drawInsetInRect:(NSRect)r;

// Pixel-style rounded button path (per button.svg).
+ (NSBezierPath *)steppedPathInRect:(NSRect)r;

// Mac-style window control box (per *_box.svg). glyph: 0=close, 1=collapse, 2=zoom.
+ (void)drawMacControlBox:(NSRect)r glyph:(int)glyph;
@end

// Pixel-accurate Platinum title bar drawing — implemented in OS9ThemeTitleBar.mm.
@interface OS9Theme (TitleBar)
// Active bar background: #262626 bottom border + #CCCCCC fill.
+ (void)drawTitleBarFrameInRect:(NSRect)r;
// INACTIVE bar background: flat #D6D6D6, no pinstripe (buttons + icons drawn by title bar).
+ (void)drawTitleBarInactiveInRect:(NSRect)r;
// Pinstripe grip band: fixed 13px tall, centered in r. mirrored=YES flips the lit edge so the LEFT
// band faces the centered title (the two bands read as a symmetric pair).
+ (void)drawTitleGripInRect:(NSRect)r mirrored:(BOOL)mirrored;
// Square bevel title button. glyph: 0=close (empty), 1=zoom (small square top-left), 2=collapse (windowshade bars).
+ (void)drawTitleButtonInRect:(NSRect)r glyph:(int)glyph pressed:(BOOL)pressed;

// ▾ arrow + divider line for dropdown (method/env) — per dropdown.svg.
+ (void)drawDropdownArrowInRect:(NSRect)r;

// Draw platinum striped title bar. Stripes drawn only within stripesRect (between the 2 icon clusters),
// the band background still covers all of r.
+ (void)drawStripedTitleInRect:(NSRect)r stripesInRect:(NSRect)stripesRect active:(BOOL)active;

// Small serrated-corner path (for URL input field / status line).
+ (NSBezierPath *)serratedPathInRect:(NSRect)r;

// Chunky aliased (pixel-crisp) ✓ stroked inside r; drawn for FLIPPED (top-left origin) views.
+ (void)drawCheckInRect:(NSRect)r color:(NSColor *)c;
@end
