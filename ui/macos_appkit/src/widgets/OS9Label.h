// Small OS9-style label (borderless NSTextField) + vertically centered variant.
#import <Cocoa/Cocoa.h>

// Small OS9-style label (borderless NSTextField).
NSTextField *OS9Label(NSString *text);

// Like OS9Label but VERTICALLY CENTERED (for status lines taller than the text).
NSTextField *OS9CenteredLabel(NSString *text);
