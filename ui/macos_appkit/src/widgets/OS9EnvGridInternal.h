#import <Cocoa/Cocoa.h>
#import "widgets/OS9EnvGrid.h"

@class OS9Toggle;

static const CGFloat kHeaderH  = 22;
static const CGFloat kRowH     = 24;
static const CGFloat kAliasW0  = 170;   // default Alias column width
static const CGFloat kColW0    = 150;   // default env column width
static const CGFloat kAddRowH  = 24;    // "+ alias" row at bottom of table
static const CGFloat kGlyph    = 13;    // × glyph hot-zone
static const CGFloat kMinColW  = 60;    // minimum column width when dragging
static const CGFloat kGrabW    = 5;     // grab zone for column resize (each side of divider)
static const CGFloat kToggleH  = 18;    // secret OS9Toggle height (fits the label)
static NSString *const kSecretLabel = @"Enc";   // encrypt-at-rest toggle label (EnvKey.secret)

// Shared draw helpers (defined in OS9EnvGridDraw.mm).
void DrawClose(NSRect box, NSColor *c);
void DrawPlus(NSRect box, NSColor *c);
NSDictionary *TextAttrs(NSColor *fg);
NSString *Ellipsize(NSString *s, CGFloat maxW, NSDictionary *attrs);
void DrawCellText(NSString *s, NSRect cell, NSColor *fg);

#pragma mark - internal flipped subviews

@interface OS9EnvGridBody : NSView
@property(nonatomic, weak) OS9EnvGrid *owner;
@end
@interface OS9EnvGridHeader : NSView
@property(nonatomic, weak) OS9EnvGrid *owner;
@end

// The ONE class extension — ivars live here so all 3 TUs (main + Draw + Input) see them.
@interface OS9EnvGrid () {
    OS9EnvGridHeader *_header;
    NSScrollView *_scroll;
    OS9EnvGridBody *_body;

    CGFloat _aliasW;
    NSMutableArray<NSNumber *> *_colW;   // width of each env column (parallel to _envNames)
    BOOL _autoFitCols;                   // YES: distribute env columns evenly over available width
    NSInteger _hoverRow;
    NSInteger _hoverEnvCol;              // env column hovered in header (-1 = none) -> shows × delete
    NSArray<NSArray<NSString *> *> *_cellCache;   // value matrix [row][col], rebuilt on reloadData
    NSArray<NSNumber *> *_secretCache;            // per-row secret flag, rebuilt on reloadData
    NSMutableArray<OS9Toggle *> *_secretToggles;  // one live OS9Toggle per alias row (Secret column)

    // Explicit backings for the public properties (auto-@synthesize reuses them -> visible to categories).
    NSArray<NSString *> *_envNames;
    NSArray<NSString *> *_aliases;
    NSInteger _selectedRow;
    BOOL _protectedFirstColumn;
}
// geometry (per-column widths) — defined in OS9EnvGrid.mm, called by the Draw/Input categories.
- (CGFloat)envWidth:(NSInteger)e;
- (CGFloat)envContentX:(NSInteger)e;
- (NSRect)envRectAtIndex:(NSInteger)e height:(CGFloat)h;
- (CGFloat)trailingX;
- (CGFloat)trailingColW;
- (CGFloat)contentWidth;
- (CGFloat)bodyHeight;
- (CGFloat)scrollX;
- (NSString *)displayForEnv:(NSInteger)e;
- (void)invalidateBodyRow:(NSInteger)row;   // invalidate EXACTLY 1 body row (hover/selection)
@end

// Cross-TU-called selectors of the category TUs.
@interface OS9EnvGrid (Draw)
- (void)drawBodyIn:(NSView *)v dirty:(NSRect)dirty;
- (void)drawHeaderIn:(NSView *)v;
- (NSRect)closeBoxInRect:(NSRect)r;
- (NSRect)closeBoxInAliasRowAtY:(CGFloat)y;
@end

@interface OS9EnvGrid (Input)
- (void)bodyMouseDown:(NSEvent *)e;
- (void)headerMouseDown:(NSEvent *)e;
- (void)setHoverRowFromBodyEvent:(NSEvent *)e;
- (void)headerMouseMoved:(NSEvent *)e;
- (void)headerMouseExited;
@end
