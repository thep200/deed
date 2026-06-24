// OS9Toggle — a retro Platinum slide switch (label + square sliding knob). Click anywhere to flip it
// left/right (off/on). Fires target/action on change, like NSButton. Used for the gRPC TLS toggle.
#import <Cocoa/Cocoa.h>

@interface OS9Toggle : NSControl
@property(nonatomic, copy) NSString *label;   // text shown to the left of the switch (e.g. "TLS")
@property(nonatomic) BOOL on;                 // set programmatically; flipped on click
- (instancetype)initWithLabel:(NSString *)label target:(id)target action:(SEL)action;
// Snug width: the knob just fits the label (small padding), track is two such cells.
- (CGFloat)preferredWidth;
@end
