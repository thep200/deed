#import "app/DeedConfig.h"
#import "app/DeedConfigData.h" // kDeedEnvData — nội dung .env nhúng lúc build (TỰ SINH)

@implementation DeedConfig {
    NSMutableDictionary<NSString *, NSString *> *_kv;
}

+ (instancetype)shared {
    static DeedConfig *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[DeedConfig alloc] initLoad]; });
    return s;
}

- (instancetype)initLoad {
    if ((self = [super init])) {
        _kv = [NSMutableDictionary dictionary];
        // Cấu hình là compile-time constant: parse chuỗi đã nhúng vào binary lúc build,
        // không đọc file .env khi chạy.
        [self parseContent:[NSString stringWithUTF8String:kDeedEnvData]];
    }
    return self;
}

- (void)parseContent:(NSString *)content {
    for (NSString *raw in [content componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (line.length == 0 || [line hasPrefix:@"#"]) continue;
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *k = [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSString *v = [[line substringFromIndex:eq.location + 1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        // Bỏ inline comment "value # ghi chú": cắt từ dấu # ĐẦU TIÊN có KHOẢNG TRẮNG đứng trước
        // (chuẩn dotenv). Giữ được value chứa '#' liền kề (vd màu #DDDDDD đứng một mình).
        // KHÔNG strip -> "classic # ..." không khớp "classic" -> rơi nhầm về kiểu mặc định.
        NSRange c1 = [v rangeOfString:@" #"], c2 = [v rangeOfString:@"\t#"];
        NSUInteger cut = NSNotFound;
        if (c1.location != NSNotFound) cut = c1.location;
        if (c2.location != NSNotFound && (cut == NSNotFound || c2.location < cut)) cut = c2.location;
        if (cut != NSNotFound)
            v = [[v substringToIndex:cut] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        // bỏ nháy bao quanh nếu có
        if (v.length >= 2 && ([v hasPrefix:@"\""] || [v hasPrefix:@"'"]))
            v = [v substringWithRange:NSMakeRange(1, v.length - 2)];
        if (k.length) _kv[k] = v;
    }
}

- (NSString *)stringFor:(NSString *)key def:(NSString *)def {
    NSString *v = _kv[key];
    return (v && v.length) ? v : def;
}

- (CGFloat)floatFor:(NSString *)key def:(CGFloat)def {
    NSString *v = _kv[key];
    return (v && v.length) ? (CGFloat)[v doubleValue] : def;
}

- (NSInteger)intFor:(NSString *)key def:(NSInteger)def {
    NSString *v = _kv[key];
    return (v && v.length) ? (NSInteger)[v integerValue] : def;
}

- (BOOL)boolFor:(NSString *)key def:(BOOL)def {
    NSString *v = _kv[key];
    if (!v || !v.length) return def;
    v = v.lowercaseString;
    return [v isEqualToString:@"1"] || [v isEqualToString:@"true"] ||
           [v isEqualToString:@"yes"] || [v isEqualToString:@"on"];
}

- (NSString *)appName { return [self stringFor:@"APP_NAME" def:@"deed"]; }

@end
