#import <Cocoa/Cocoa.h>

#import "widgets/OS9EnvGrid.h"

namespace core { class Engine; }

// Ma trận ENV (hàng=alias, cột=env; cột 0 = base hiển thị "Local"). SPEC §T1–T5.
// Tự vẽ bằng OS9EnvGrid (không NSTableView). Action inline trong bảng (không còn
// dải nút bottom). Nhúng vào màn Config: vend ra một NSView.
@interface EnvWindowController : NSObject <OS9EnvGridDelegate>
- (instancetype)initWithEngine:(core::Engine *)engine;
- (NSView *)view;     // build (lazy) + trả container để nhúng
- (void)reload;       // nạp lại từ store
- (void)save;         // ghi các env đã sửa (atomic, plaintext)
- (void)layout;       // sắp xếp lại subview theo bounds hiện tại của view
@end
