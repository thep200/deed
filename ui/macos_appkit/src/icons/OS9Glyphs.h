// OS9Glyphs — icon vẽ tay bằng NSBezierPath (tách từ OS9Widgets).
#import <Cocoa/Cocoa.h>

// Vẽ icon bánh răng cổ điển (cog) cỡ size — dùng cho nút Setting.
NSImage *OS9GearImage(CGFloat size);

// Icon "send" (máy bay giấy hướng phải) — thay label nút Send.
NSImage *OS9SendImage(CGFloat size);
// Icon folder OS9 (màu xanh Platinum, có tab) — dùng cho title bar trạng thái Inactive.
NSImage *OS9FolderImage(CGFloat size);
// Icon loading (spinner nan hoa) tại pha quay phase∈[0,1) — animate bằng timer.
NSImage *OS9SpinnerImage(CGFloat size, CGFloat phase);
// Mảng `frameCount` ảnh spinner dựng sẵn (pha chia đều) + CACHE theo (size,frameCount).
// Dùng cho animation: timer chỉ index vào mảng thay vì cấp phát NSImage mỗi tick.
NSArray<NSImage *> *OS9SpinnerFrames(CGFloat size, int frameCount);
