// Pinstriped title bar + close button, window is draggable.
#import <Cocoa/Cocoa.h>

@interface OS9TitleBar : NSView
@property(nonatomic, copy) NSString *title;
@property(nonatomic, weak) id closeTarget;
@property(nonatomic) SEL closeAction;
@property(nonatomic, weak) id zoomTarget;     // nil -> default performZoom
@property(nonatomic) SEL zoomAction;
@property(nonatomic, weak) id collapseTarget; // nil -> default miniaturize
@property(nonatomic) SEL collapseAction;

// Outer inset of title icons (outer edge of close/hide to window edge). Used so the main screen
// aligns the outer edge of panes/buttons with the title icons on both sides.
+ (CGFloat)iconSideInset;
@end
