// Custom dropdown: SQUARE-CORNERED item list (no system NSMenu), shown just
// below (or above if no room) the anchor. onPick(index) on select; auto-closes on outside click/Esc.
#import <Cocoa/Cocoa.h>

void OS9ShowDropdown(NSArray<NSString *> *items, NSInteger selected, NSView *anchor,
                     void (^onPick)(NSInteger index));
