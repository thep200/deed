// DeedConfig — đọc cấu hình từ file .env để chỉnh giao diện không cần sửa code.
// Thứ tự tìm .env: $DEED_ENV > ./.env (cwd, tiện khi dev) > Resources/.env (trong bundle)
//                  > ~/.deed.env. Dòng dạng KEY=VALUE, bỏ qua dòng trống và '#'.
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
