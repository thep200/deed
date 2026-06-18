// StressRunner — in-app stress driver (STRESS_TEST.md §5). CHỈ debug, KHÔNG ship release.
// Gate: build flag DEED_DEBUG_TOOLS + env DEED_STRESS=1. Chạy trên MAIN THREAD qua timer,
// lặp N vòng (seed + delay nhỏ), mỗi vòng chọn ngẫu nhiên một thao tác controller đi đúng
// đường vòng đời first-responder/Scintilla/window đã gây crash; log RAM mỗi vòng + idle checkpoint.
#import <Cocoa/Cocoa.h>

#if DEED_DEBUG_TOOLS

@class MainWindowController;

@interface StressRunner : NSObject

// Bật khi env DEED_STRESS=1. AppController gọi để quyết có chạy runner không.
+ (BOOL)enabledFromEnv;

- (instancetype)initWithController:(MainWindowController *)wc;

// Đọc DEED_STRESS_ITERS / DEED_STRESS_SEED / DEED_STRESS_LOG / DEED_STRESS_IDLE_EVERY từ env,
// bootstrap collection tạm rồi bắt đầu vòng lặp. Tự dừng sau khi đủ số vòng.
- (void)start;

@end

#endif // DEED_DEBUG_TOOLS
