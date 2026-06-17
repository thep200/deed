#import "app/DeedConfig.h"

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
        [self loadFromCandidates];
    }
    return self;
}

- (void)loadFromCandidates {
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    const char *envp = getenv("DEED_ENV");
    if (envp && *envp) [paths addObject:[NSString stringWithUTF8String:envp]];
    [paths addObject:[[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:@".env"]];
    NSString *res = [[NSBundle mainBundle] pathForResource:@"env" ofType:@""]; // Resources/.env -> "env"
    if (res) [paths addObject:res];
    NSString *bundleEnv = [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:@".env"];
    if (bundleEnv) [paths addObject:bundleEnv];
    [paths addObject:[NSHomeDirectory() stringByAppendingPathComponent:@".deed.env"]];

    for (NSString *p in paths) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:p]) {
            [self parseFile:p];
            NSLog(@"[deed] loaded config: %@", p);
            return; // dùng file đầu tiên tìm thấy
        }
    }
}

- (void)parseFile:(NSString *)path {
    NSString *content = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    for (NSString *raw in [content componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (line.length == 0 || [line hasPrefix:@"#"]) continue;
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *k = [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSString *v = [[line substringFromIndex:eq.location + 1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
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

- (BOOL)boolFor:(NSString *)key def:(BOOL)def {
    NSString *v = _kv[key];
    if (!v || !v.length) return def;
    v = v.lowercaseString;
    return [v isEqualToString:@"1"] || [v isEqualToString:@"true"] ||
           [v isEqualToString:@"yes"] || [v isEqualToString:@"on"];
}

- (NSString *)appName { return [self stringFor:@"APP_NAME" def:@"deed"]; }

@end
