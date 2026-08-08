// .env values are EMBEDDED at build time — no .env needed at runtime; edit .env + rebuild to change.
#import <Cocoa/Cocoa.h>

@interface DeedConfig : NSObject
+ (instancetype)shared;

- (NSString *)stringFor:(NSString *)key def:(NSString *)def;
- (CGFloat)floatFor:(NSString *)key def:(CGFloat)def;
- (NSInteger)intFor:(NSString *)key def:(NSInteger)def;
- (BOOL)boolFor:(NSString *)key def:(BOOL)def;

// Common convenience accessors.
- (NSString *)appName;          // APP_NAME (default "deed")
@end
