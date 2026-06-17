// Cửa sổ kiểu OS9: borderless -> góc VUÔNG (không bo tròn như titled window),
// nhưng vẫn nhận key/main + kéo/resize. Dùng khi muốn look retro vuông vức.
#import <Cocoa/Cocoa.h>

@interface OS9Window : NSWindow
@end
