// OS9-style context menu: a self-drawn overlay (platinum box, SQUARE corners, black border) —
// matches the OS9 dropdown look exactly (NOT a system NSMenu, so no rounded corners / shadow).
#import <Cocoa/Cocoa.h>

// One row of a retro context menu. Use +entry: / +separator.
@interface OS9MenuEntry : NSObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) void (^action)(void);
@property(nonatomic) BOOL separator;
@property(nonatomic) BOOL checked;   // draws a ✓ in the left gutter (like the dropdown's selected row)
+ (instancetype)entry:(NSString *)title action:(void (^)(void))action;
+ (instancetype)separator;
@end

// Pop a retro context menu anchored at `windowPoint` (window coords) in `anchor`'s window.
// Runs the picked entry's action. Modeless overlay (dismiss: pick / click-away / Esc).
void OS9ShowContextMenu(NSArray<OS9MenuEntry *> *entries, NSView *anchor, NSPoint windowPoint);
