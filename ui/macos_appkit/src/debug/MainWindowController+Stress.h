// MainWindowController(Stress) — API debug cho StressRunner (STRESS_TEST.md §5).
// Cài đặt nằm TRONG MainWindowController.mm dưới `#if DEED_DEBUG_TOOLS` (cần truy cập ivar).
// KHÔNG biên dịch vào release.
#import "windows/MainWindowController.h"

#if DEED_DEBUG_TOOLS

@interface MainWindowController (Stress)

// Dựng collection tạm + mở + tạo sẵn vài request (để runner tự chạy không cần user mở folder).
- (void)stressBootstrap;

// Danh sách relPath request hiện có (quét cây). Rỗng nếu chưa mở collection.
- (NSArray<NSString *> *)stressRequestRels;
- (NSString *)stressOpenRequestId;          // id request đang mở (rỗng nếu idle)
- (uint64_t)stressRamCacheBytes;

// Các thao tác ĐI ĐÚNG đường vòng đời đã gây crash (first-responder / Scintilla / window):
- (void)stressLoadRel:(NSString *)rel;       // load + đặt first responder vào URL (input context thật)
- (void)stressSwitchRandom:(uint32_t)r;      // chuyển sang request khác (đường teardown §2.1)
- (void)stressTypeRandom:(uint32_t)r;        // focus URL/editor rồi chèn text
- (void)stressToggleRandomFolder:(uint32_t)r;// expand/collapse folder (lazy scan)
- (void)stressEnterEnv;                      // mở config ENV (field editor + table)
- (void)stressEnterSettings;                 // mở config Settings (SciTextView)
- (void)stressExitConfig;                    // back (resign + teardown editor §2.1)
- (void)stressPickRandomEnv:(uint32_t)r;     // đổi env active
- (void)stressInjectResponse:(BOOL)large;    // bơm response giả (nhỏ / ~20MB) vào view + cache
- (void)stressRenameAutoDismiss:(uint32_t)r; // present rename dialog rồi tự huỷ (đường §2.3)
- (void)stressGoIdle;                        // về trạng thái không mở request (baseline)

@end

#endif // DEED_DEBUG_TOOLS
