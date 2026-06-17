// Dropdown tuỳ biến: danh sách item GÓC VUÔNG (không dùng NSMenu hệ thống), hiện ngay
// dưới (hoặc trên nếu hết chỗ) anchor. onPick(index) khi chọn; tự đóng khi click ngoài/Esc.
#import <Cocoa/Cocoa.h>

void OS9ShowDropdown(NSArray<NSString *> *items, NSInteger selected, NSView *anchor,
                     void (^onPick)(NSInteger index));
