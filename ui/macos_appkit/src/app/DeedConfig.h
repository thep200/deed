// DeedConfig — UI configuration. Values come from .env (repo root) but are EMBEDDED into
// the binary at build time (compile-time constant), so the app does NOT need a .env at runtime.
// To change config: edit .env and rebuild. Lines are KEY=VALUE; blank lines and '#' are ignored.
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
