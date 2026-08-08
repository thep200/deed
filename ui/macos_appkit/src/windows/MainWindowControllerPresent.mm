#import "windows/MainWindowControllerPrivate.h"

@implementation MainWindowController (Present)

#pragma mark Live request timing (elapsed + size)

// Reset live counters + start the status ticker. Cheap: ONE repeating timer (~20Hz) that recomputes the
// line from a monotonic start mark + running counters — chunks don't write the label themselves (perf).
- (void)beginRequestTiming {
    _reqStartTime = NSProcessInfo.processInfo.systemUptime;   // monotonic (immune to wall-clock changes)
    _streamBytes = 0;
    _streamEvents = 0;
    [self stopLiveTimer];
    __weak MainWindowController *ws = self;
    _liveTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
        MainWindowController *s = ws; if (!s) { [t invalidate]; return; }
        [s liveTick];
    }];
    _liveTimer.tolerance = 0.02;   // let the runloop coalesce wakeups (perf)
    [self liveTick];               // paint frame 0 now (don't wait for the first interval)
}

- (void)stopLiveTimer { [_liveTimer invalidate]; _liveTimer = nil; }

// One live status frame: elapsed ms always; for streams also the running size + event count.
- (void)liveTick {
    if (!_sending) return;   // defensive: never paint after the request settled
    NSString *t = [self elapsedText:[self measuredElapsedMs]];
    if (_streaming)
        _statusLabel.stringValue = [NSString stringWithFormat:StrFmtStreamLive, t,
                                    [self humanSize:_streamBytes], (unsigned long long)_streamEvents];
    else
        _statusLabel.stringValue = [NSString stringWithFormat:StrFmtReqElapsed, t];
    _statusLabel.textColor = [NSColor blackColor];
}

- (NSString *)humanSize:(int64_t)bytes {
    return (bytes >= 1024) ? [NSString stringWithFormat:@"%.1fkb", bytes / 1024.0]
                           : [NSString stringWithFormat:@"%lldb", (long long)bytes];
}

// Elapsed-time display: milliseconds up to 1000ms, then switch to WHOLE seconds, no decimals
// (e.g. 850ms -> "850ms", 1234ms -> "1s", 2999ms -> "2s"). Single source so the live ticker +
// final/stream/error status all format alike.
- (NSString *)elapsedText:(long long)ms {
    if (ms < 0) ms = 0;
    if (ms > 1000) return [NSString stringWithFormat:@"%llds", ms / 1000];
    return [NSString stringWithFormat:@"%lldms", ms];
}

// Elapsed measured by the UI's own monotonic start mark (set in beginRequestTiming, kept after
// finishSending). Used when the settled response/error carries no duration — e.g. a user-pressed Cancel,
// where the sender reports 0ms — so the status shows the REAL time instead of 0ms.
- (long long)measuredElapsedMs {
    long long ms = (long long)((NSProcessInfo.processInfo.systemUptime - _reqStartTime) * 1000.0);
    return ms < 0 ? 0 : ms;
}

// Compute the display buffers (format JSON body/headers/cookie) — the HEAVY part, does NOT touch UI ->
// callable from a background thread. Depends only on params (r/type/prettyMode), reads no ivars.
- (NSArray<NSString *> *)computeResponseBuffersFor:(const core::domain::ApiResponse &)r
                                              type:(core::RequestType)type
                                        prettyMode:(int)prettyMode {
    // Buffer count/order must match the binder's responseTabTitles — both live in the same binder now.
    NSString *body = [self applyView:r.body mode:prettyMode]; // per Pretty/Raw/Encode/Decode
    return [TypeUiFor(type) responseBuffers:r body:body];
}

// Attach computed buffers to UI + select the remembered tab (LIGHT, runs on main thread).
- (void)applyResponseBuffers:(NSArray<NSString *> *)bufs {
    [_respBuffers removeAllObjects];
    [_respBuffers addObjectsFromArray:bufs];
    // Schema tab (GraphQL) is NOT buffer-backed: if it is the remembered tab and a schema is cached,
    // keep showing it (a send must not yank the user off the schema). Unfetched -> clamp below -> tab 0.
    NSInteger si = [_respTabTitles indexOfObject:StrTabSchema];
    if (si != NSNotFound && _gqlSchemaFetched && [_rightPaneActiveTabKey isEqualToString:StrTabSchema] &&
        [self requestType] == core::RequestType::GraphQl) {
        _activeRespTab = si;
        [self displayGqlSchemaPane];
        [self highlightActiveTab:_respTabButtons active:si];
        return;
    }
    // Reapply the right pane's remembered tab (semantic key); no match -> first tab.
    NSInteger ri = [self tabIndexForKey:_rightPaneActiveTabKey inTitles:_respTabTitles];
    if (ri >= (NSInteger)_respBuffers.count) ri = 0;
    _activeRespTab = ri;
    _respText.string = _respBuffers.count ? _respBuffers[ri] : @"";
    [self applyRespPaneLanguageFor:(_respBuffers.count ? _respBuffers[ri] : @"")];
    [self highlightActiveTab:_respTabButtons active:ri];
}

// Synchronous: used for cheap re-renders (change view mode, change tab, stress).
- (void)rebuildResponseBuffers {
    [self applyResponseBuffers:[self computeResponseBuffersFor:_lastResp type:[self requestType] prettyMode:_prettyMode]];
}

// Asynchronous: format the response OFF the main thread then apply on main (large response won't freeze UI).
// Drop the result if a new request arrived (compare _currentHandle) -> avoid showing stale buffers.
- (void)rebuildResponseBuffersAsync {
    core::domain::ApiResponse r = _lastResp;    // copy for safe background use (main won't mutate concurrently)
    core::RequestType type = [self requestType];
    int pm = _prettyMode;
    uint64_t handle = _currentHandle;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws; if (!s) return;
        NSArray<NSString *> *bufs = [s computeResponseBuffersFor:r type:type prettyMode:pm];
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws; if (!s2) return;
            if (handle != s2->_currentHandle) return;   // a new request was sent/received -> drop stale buffers
            [s2 applyResponseBuffers:bufs];
        });
    });
}

#pragma mark Status line

- (void)updateStatus:(NSString *)text {
    _statusLabel.stringValue = text ?: @"";
    _statusLabel.textColor = [NSColor blackColor];
}

// Time HH:mm:ss.SSS (millisecond precision) from epoch ms. <=0 -> placeholder.
- (NSString *)clockFromEpochMs:(int64_t)ms {
    // NSDateFormatter is NOT thread-safe and this instance is shared. MAIN-THREAD ONLY — callers run on
    // the main queue (status updates). Do NOT call from computeResponseBuffersFor: or any background worker.
    NSAssert([NSThread isMainThread], @"clockFromEpochMs: must be called on the main thread (shared formatter)");
    if (ms <= 0) return @"--:--:--.---";
    static NSDateFormatter *fmt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ fmt = [NSDateFormatter new]; fmt.dateFormat = @"HH:mm:ss.SSS"; });
    return [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:ms / 1000.0]];
}

// status | size | time | start - end. endMs = time response received; start = end - elapsed.
- (void)updateStatusFromResponse:(const core::domain::ApiResponse &)r error:(BOOL)isErr endMs:(int64_t)endMs {
    long elapsedMs = (long)r.elapsed.count();
    NSString *code = r.statusCode ? [NSString stringWithFormat:@"%d", r.statusCode] : StrOK;
    BOOL bad = (r.statusCode >= 400);
    if (!isErr) {
        // Binder override for protocols whose status code is not HTTP (LDAP: "rc=N · VERDICT").
        NSString *c2 = nil;
        BOOL b2 = NO;
        if ([TypeUiFor([self requestType]) statusLine:r code:&c2 bad:&b2]) {
            code = c2 ?: code;
            bad = b2;
        }
    }
    NSString *size = [self humanSize:(int64_t)r.body.size()];
    int64_t startMs = (endMs > 0) ? endMs - (int64_t)elapsedMs : 0;   // derive the start mark
    NSString *range = [NSString stringWithFormat:@"%@ - %@",
                       [self clockFromEpochMs:startMs], [self clockFromEpochMs:endMs]];
    _statusLabel.stringValue = [NSString stringWithFormat:@"%@ | %@ | %@ | %@", code, size,
                                                          [self elapsedText:elapsedMs], range];
    _statusLabel.textColor = bad ? [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0]
                                 : [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1.0];
}

@end
