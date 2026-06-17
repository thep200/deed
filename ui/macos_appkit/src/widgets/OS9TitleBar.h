// Title bar kẻ sọc + nút close, kéo cửa sổ được.
#import <Cocoa/Cocoa.h>

@interface OS9TitleBar : NSView
@property(nonatomic, copy) NSString *title;
@property(nonatomic, weak) id closeTarget;
@property(nonatomic) SEL closeAction;
@property(nonatomic, weak) id zoomTarget;     // nil -> performZoom mặc định
@property(nonatomic) SEL zoomAction;
@property(nonatomic, weak) id collapseTarget; // nil -> miniaturize mặc định
@property(nonatomic) SEL collapseAction;
@end
