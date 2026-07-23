// OS9Theme — Platinum (Mac OS 9) color palette + bevel/inset drawing helpers (README §5).
// Custom look, does NOT use default AppKit controls (modern Aqua render).
#import <Cocoa/Cocoa.h>

@interface OS9Theme : NSObject
// Theme selection (Settings "theme": "light" | "dark") — set ONCE at startup BEFORE building widgets;
// changing it later needs an app restart (no live re-style).
+ (void)setThemeName:(NSString *)name;   // "dark" -> dark palette; anything else -> light
+ (BOOL)isDarkTheme;                     // for the rare branch outside tokens (NSTextField caret...)

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

// --- Semantic tokens (theme-aware). Use these instead of NSColor literals in widgets/windows. ---
+ (NSColor *)textPrimary;      // main ink: labels, rows, field text, toast text (black / near-white)
+ (NSColor *)textSecondary;    // muted text: editor gutter numbers
+ (NSColor *)insetBg;          // sunken field/editor/tree/grid background (white / near-black)
+ (NSColor *)titleTextActive;  // title bar text, active window
+ (NSColor *)titleTextInactive;
// menus (OS9Dropdown / OS9StyleMenu)
+ (NSColor *)menuHoverBg;      // navy hover row fill
+ (NSColor *)menuHoverText;    // text/check on hover row
+ (NSColor *)menuSeparator;    // separator + disabled text gray
+ (NSColor *)menuBorder;       // menu panel border
// status line (Send) + dialog error label
+ (NSColor *)statusOk;
+ (NSColor *)statusError;
// toast
+ (NSColor *)toastBg;
+ (NSColor *)toastOk;
+ (NSColor *)toastError;
+ (NSColor *)toastInfo;
// scroller (OS9Scroller thumb)
+ (NSColor *)scrollerThumb;
+ (NSColor *)scrollerBorder;
+ (NSColor *)scrollerGripDark;
+ (NSColor *)scrollerGripLight;
// Scintilla editor palette (SciTextView / JsonEditorBehavior)
+ (NSColor *)editorBg;
+ (NSColor *)editorFg;         // default text + operator + caret
+ (NSColor *)editorString;
+ (NSColor *)editorNumber;     // number + keyword
+ (NSColor *)editorProperty;
+ (NSColor *)editorSelectionBg;
+ (NSColor *)editorBraceFg;    // matched-brace highlight fg/bg
+ (NSColor *)editorBraceBg;
+ (NSColor *)editorBraceBadFg; // unmatched brace
// pictorial glyphs (OS9Glyphs strokes, TreeViews 3D box)
+ (NSColor *)glyphStroke;
+ (NSColor *)glyphBoxFill;
+ (NSColor *)glyphBoxHighlight;
+ (NSColor *)glyphBoxOutline;
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
+ (void)setClassicButtonStyle:(BOOL)classic;   // kept for compatibility (classic<->new)
+ (void)drawButtonInRect:(NSRect)r pressed:(BOOL)pressed isDefault:(BOOL)isDefault;
// Text color matching button state (line style inverts fill when pressed -> white text).
+ (NSColor *)buttonFGPressed:(BOOL)pressed enabled:(BOOL)enabled;
// Draw inset (sunken) border for input field: dark top-left, light bottom-right.
+ (void)drawInsetInRect:(NSRect)r;
// Draw platinum striped title bar. Stripes drawn only within stripesRect (between the 2 icon clusters),
// the band background still covers all of r.
+ (void)drawStripedTitleInRect:(NSRect)r stripesInRect:(NSRect)stripesRect active:(BOOL)active;

// Pixel-style rounded button path (per button.svg).
+ (NSBezierPath *)steppedPathInRect:(NSRect)r;

// Small serrated-corner path (for URL input field / status line).
+ (NSBezierPath *)serratedPathInRect:(NSRect)r;

// Mac-style window control box (per *_box.svg). glyph: 0=close, 1=collapse, 2=zoom.
+ (void)drawMacControlBox:(NSRect)r glyph:(int)glyph;

// === OS9 Platinum title bar (PROMPT_os9_titlebar_objcpp.md — pixel-accurate draw) ===
// Active bar background: #262626 bottom border + #CCCCCC fill.
+ (void)drawTitleBarFrameInRect:(NSRect)r;
// INACTIVE bar background: flat #D6D6D6, no pinstripe (buttons + icons drawn by title bar).
+ (void)drawTitleBarInactiveInRect:(NSRect)r;
// Pinstripe grip band: #DDDDDD fill, #EEEEEE/#C5C5C5 edges, #999999 line every 2px;
// fixed 13px tall, vertically centered in r, stretches with r.width.
// mirrored=NO: light edge left, dark edge right (default). mirrored=YES: swapped — so the LEFT band
// can face its lit edge toward the centered title (the two bands read as a symmetric pair, not one run).
+ (void)drawTitleGripInRect:(NSRect)r mirrored:(BOOL)mirrored;
// Title button (close/zoom/collapse) — square bevel box with side r.size.width.
// Structure (outer→inner, each layer 1px): outer bevel (TL #808080 / BR #FFFFFF, sunken) ->
// black frame #262626 -> vertical gradient face #C9C9C9(top)→#F1F1F1(bottom) -> inner bevel
// (TL #FFFFFF / BR #9A9A9A, raised) -> glyph #262626.
// glyph: 0=close (empty), 1=zoom (small square top-left, shares an edge),
//        2=collapse (2 horizontal bars touching both edges -> 3 bands, windowshade).
// pressed=YES -> overlay #353535→#9C9C9C @0.8 diagonal TL→BR on the face (mouse-down).
+ (void)drawTitleButtonInRect:(NSRect)r glyph:(int)glyph pressed:(BOOL)pressed;

// ▾ arrow + divider line for dropdown (method/env) — per dropdown.svg.
+ (void)drawDropdownArrowInRect:(NSRect)r;

// Vintage Mac OS 9 checkmark: a chunky, aliased (pixel-crisp) ✓ stroked inside `r`, in color `c`.
// Used for selection ticks (dropdown/menu) and the env "secret" checkbox. Drawn for FLIPPED views
// (top-left origin) — all our self-drawn views are flipped.
+ (void)drawCheckInRect:(NSRect)r color:(NSColor *)c;
@end
