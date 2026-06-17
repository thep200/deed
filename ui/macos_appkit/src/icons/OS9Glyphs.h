// OS9Glyphs — icon vẽ tay bằng NSBezierPath (tách từ OS9Widgets).
#import <Cocoa/Cocoa.h>

// Vẽ icon bánh răng cổ điển (cog) cỡ size — dùng cho nút Setting.
NSImage *OS9GearImage(CGFloat size);

// Icon "send" (máy bay giấy hướng phải) — thay label nút Send.
NSImage *OS9SendImage(CGFloat size);
// Icon loading (spinner nan hoa) tại pha quay phase∈[0,1) — animate bằng timer.
NSImage *OS9SpinnerImage(CGFloat size, CGFloat phase);
