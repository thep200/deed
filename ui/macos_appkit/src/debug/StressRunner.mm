#import "debug/StressRunner.h"

#if DEED_DEBUG_TOOLS

#import "debug/MainWindowControllerStress.h"

#include <memory>
#include <random>
#include <string>

#include "core/infra/platform/mem_probe.hpp"

@implementation StressRunner {
    __weak MainWindowController *_wc;
    std::unique_ptr<std::mt19937> _rng;
    std::unique_ptr<core::memprobe::StructuredLogger> _logger;
    long long _iters;
    long long _idleEvery;
    long long _cur;
    NSTimeInterval _delay;
}

+ (BOOL)enabledFromEnv {
    const char *s = getenv("DEED_STRESS");
    return s && s[0] == '1';
}

- (instancetype)initWithController:(MainWindowController *)wc {
    if ((self = [super init])) {
        _wc = wc;
        _cur = 0;
    }
    return self;
}

static long long EnvLL(const char *key, long long def) {
    const char *s = getenv(key);
    if (!s || !*s) return def;
    try { return std::stoll(s); } catch (...) { return def; }
}

- (void)start {
    unsigned seed = (unsigned)EnvLL("DEED_STRESS_SEED", 42);
    _iters = EnvLL("DEED_STRESS_ITERS", 20000);
    _idleEvery = EnvLL("DEED_STRESS_IDLE_EVERY", 500);
    _delay = (NSTimeInterval)EnvLL("DEED_STRESS_DELAY_MS", 5) / 1000.0;
    _rng = std::make_unique<std::mt19937>(seed);

    const char *logPath = getenv("DEED_STRESS_LOG");
    if (logPath && *logPath) {
        _logger = std::make_unique<core::memprobe::StructuredLogger>(logPath);
        if (!_logger->ok()) NSLog(@"[stress] could not open log %s", logPath);
    }

    NSLog(@"[stress] start iters=%lld seed=%u idleEvery=%lld delayMs=%.0f",
          _iters, seed, _idleEvery, _delay * 1000.0);
    [_wc stressBootstrap];
    [self scheduleNext];
}

- (void)scheduleNext {
    if (_cur >= _iters) { [self finish]; return; }
    // Loop via a main-thread timer (do NOT block the run loop) -> AppKit processes updateWindows
    // between ops, the exact condition that reproduces the input-context crash.
    [self performSelector:@selector(tick) withObject:nil afterDelay:_delay];
}

- (void)logOp:(const char *)op idle:(BOOL)idle {
    if (!_logger) return;
    core::memprobe::StructuredLogger::Row r;
    r.iter = _cur;
    r.op = op;
    r.idle = idle;
    MainWindowController *wc = _wc;
    r.ramCacheBytes = wc ? [wc stressRamCacheBytes] : 0;
    r.openRequestId = (idle || !wc) ? std::string() : std::string([wc stressOpenRequestId].UTF8String ?: "");
    _logger->log(r);
}

- (void)tick {
    MainWindowController *wc = _wc;
    if (!wc) { [self finish]; return; }

    uint32_t r = (*_rng)();
    int op = r % 10;
    const char *opName = "noop";
    @try {
        switch (op) {
            case 0: case 1: opName = "switch";  [wc stressSwitchRandom:(*_rng)()]; break;
            case 2: case 3: opName = "type";    [wc stressTypeRandom:(*_rng)()]; break;
            case 4:         opName = "folder";  [wc stressToggleRandomFolder:(*_rng)()]; break;
            case 5:         opName = "env_in";  [wc stressEnterEnv]; [wc stressExitConfig]; break;
            case 6:         opName = "settings";[wc stressEnterSettings]; [wc stressExitConfig]; break;
            case 7:         opName = "pickenv"; [wc stressPickRandomEnv:(*_rng)()]; break;
            case 8:         opName = "inject";  [wc stressInjectResponse:((*_rng)() % 4 == 0)]; break;
            case 9:         opName = "rename";  [wc stressRenameAutoDismiss:(*_rng)()]; break;
        }
    } @catch (NSException *e) {
        NSLog(@"[stress] op %s exception: %@", opName, e);
    }

    [self logOp:opName idle:NO];

    if (_idleEvery > 0 && (_cur % _idleEvery) == (_idleEvery - 1)) {
        [wc stressGoIdle];
        [self logOp:"idle" idle:YES];
    }

    _cur++;
    [self scheduleNext];
}

- (void)finish {
    if (_logger) _logger->flush();
    NSLog(@"[stress] done iters=%lld final_footprint_mb=%.2f", _cur,
          (double)core::memprobe::PhysFootprintBytes() / (1024.0 * 1024.0));
    _wc = nil;
}

@end

#endif // DEED_DEBUG_TOOLS
