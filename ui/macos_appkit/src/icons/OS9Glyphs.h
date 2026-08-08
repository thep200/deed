#import <Cocoa/Cocoa.h>

// Draw a classic cog icon of given size — used for the Settings button.
NSImage *OS9GearImage(CGFloat size);

// "Send" icon (right-facing paper plane) — replaces the Send button label.
NSImage *OS9SendImage(CGFloat size);
// OS9 folder icon (Platinum blue, with tab) — used for the Inactive title bar state.
NSImage *OS9FolderImage(CGFloat size);
// Loading spinner (spoked) at rotation phase∈[0,1) — animate with a timer.
NSImage *OS9SpinnerImage(CGFloat size, CGFloat phase);
// Array of `frameCount` prebuilt spinner images (evenly spaced phases) + CACHE keyed by (size,frameCount).
// For animation: timer just indexes into the array instead of allocating an NSImage each tick.
NSArray<NSImage *> *OS9SpinnerFrames(CGFloat size, int frameCount);
