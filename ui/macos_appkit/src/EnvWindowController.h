#import <Cocoa/Cocoa.h>

namespace core { class Engine; }

// Ma trận ENV (hàng=alias, cột=env; cột Global đầu, khoá). UI spec §6.
// Giờ NHÚNG vào màn Config chung (không còn cửa sổ riêng): vend ra một NSView.
@interface EnvWindowController : NSObject <NSTableViewDataSource, NSTableViewDelegate>
- (instancetype)initWithEngine:(core::Engine *)engine;
- (NSView *)view;     // build (lazy) + trả container để nhúng
- (void)reload;       // nạp lại từ store
- (void)save;         // ghi các env đã sửa (atomic; secret -> SecretStore)
- (void)layout;       // sắp xếp lại subview theo bounds hiện tại của view
@end
