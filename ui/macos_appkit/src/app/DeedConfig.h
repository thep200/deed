// DeedConfig — cấu hình giao diện. Giá trị lấy từ .env (gốc repo) nhưng được NHÚNG vào
// binary lúc build (compile-time constant), nên app KHÔNG cần file .env khi chạy.
// Đổi cấu hình: sửa .env rồi build lại. Dòng dạng KEY=VALUE, bỏ qua dòng trống và '#'.
#import <Cocoa/Cocoa.h>

@interface DeedConfig : NSObject
+ (instancetype)shared;

- (NSString *)stringFor:(NSString *)key def:(NSString *)def;
- (CGFloat)floatFor:(NSString *)key def:(CGFloat)def;
- (NSInteger)intFor:(NSString *)key def:(NSInteger)def;
- (BOOL)boolFor:(NSString *)key def:(BOOL)def;

// Tiện ích hay dùng.
- (NSString *)appName;          // APP_NAME (mặc định "deed")
@end
