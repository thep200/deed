// Retro Platinum slide switch: click anywhere flips it; fires target/action like NSButton.
#import <Cocoa/Cocoa.h>

@interface OS9Toggle : NSControl
@property(nonatomic, copy) NSString *label;   // text shown to the left of the switch (e.g. "TLS")
@property(nonatomic) BOOL on;                 // set programmatically; flipped on click
- (instancetype)initWithLabel:(NSString *)label target:(id)target action:(SEL)action;
// Snug width: the knob just fits the label (small padding), track is two such cells.
- (CGFloat)preferredWidth;
@end
