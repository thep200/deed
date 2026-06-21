#import "windows/MainWindowController+Private.h"

@implementation MainWindowController (Send)

#pragma mark Send / Cancel

- (void)sendRequest:(id)sender {
    if (!_hasRequest || !_engine || _sending) return;
    [self parseUrlQueryIntoQueryTab];   // user typed query into URL -> split into Query tab before sync
    if (![self syncModelFromEditors:NO]) return;
    if (_model.type == core::RequestType::Grpc && _model.grpc.methodType != "unary") {
        [self toastWarn:StrToastUnaryOnly]; return;
    }
    _sending = YES;
    [self startSendSpinner];           // spinning loading icon in place of the label
    _cancelButton.enabledState = YES;
    [self relayout];
    [self updateStatus:@""];
    _currentHandle = _engine->send(_model, _bridge.get());
}

- (void)cancelClicked:(id)sender { if (_sending && _engine) _engine->cancel(_currentHandle); }

- (void)onCoreResponse:(uint64_t)handle response:(const core::ApiResponse &)resp {
    if (handle != _currentHandle) return;
    NSLog(@"[smoke] onCoreResponse status=%d bytes=%lld", resp.statusCode, (long long)resp.sizeBytes);
    [self finishSending];
    _lastResp = resp; _hasResp = YES;
    [self rebuildResponseBuffersAsync];   // format off the main thread -> large response won't freeze UI (U2)
    int64_t endMs = (int64_t)([[NSDate date] timeIntervalSince1970] * 1000.0);  // end mark = time received
    [self updateStatusFromResponse:resp error:NO endMs:endMs];
    [self cacheResponseAsync:resp forId:_currentId];   // store cache (background) — keyed by id
}

- (void)onCoreError:(uint64_t)handle error:(const core::ApiError &)err {
    if (handle != _currentHandle) return;
    NSLog(@"[smoke] onCoreError kind=%s msg=%s", core::toString(err.kind).c_str(), err.message.c_str());
    [self finishSending];
    [self displayErrorKind:err.kind message:N(err.message)];
    [self toastWarn:[NSString stringWithFormat:@"%@: %@", N(core::toString(err.kind)), N(err.message)]];
    [self cacheErrorAsync:err forId:_currentId];       // cache errors too, to restore the exact state (§7)
}

// Display the error state in the response pane (shared by new errors and cached errors).
- (void)displayErrorKind:(core::ErrorKind)kind message:(NSString *)msg {
    NSString *k = N(core::toString(kind));
    NSString *statusText = k;                                       // drop the ✕ before the error name
    if (kind == core::ErrorKind::Cancelled) statusText = StrStatusCancelled;
    else if (kind == core::ErrorKind::Network) statusText = StrStatusNetworkError;   // network error -> report clearly
    _statusLabel.stringValue = statusText;
    _statusLabel.textColor = [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0];
    _hasResp = NO;
    [_respBuffers removeAllObjects];
    for (NSUInteger i = 0; i < _respTabTitles.count; i++) [_respBuffers addObject:@""];
    if (_respBuffers.count) _respBuffers[0] = [NSString stringWithFormat:@"[%@] %@", k, msg ?: @""];
    _activeRespTab = 0;
    _respText.string = _respBuffers[0];
    [self highlightActiveTab:_respTabButtons active:0];
}

// Write cache on a BACKGROUND thread (§6: put async, don't block UI). Engine is thread-safe.
- (void)cacheResponseAsync:(const core::ApiResponse &)resp forId:(const std::string &)reqId {
    if (reqId.empty()) return;
    core::ApiResponse copy = resp;
    std::string id = reqId;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        MainWindowController *s = ws;
        if (s && s->_engine) s->_engine->putResponse(id, copy);
    });
}
- (void)cacheErrorAsync:(const core::ApiError &)err forId:(const std::string &)reqId {
    if (reqId.empty()) return;
    core::ApiError copy = err;
    std::string id = reqId;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        MainWindowController *s = ws;
        if (s && s->_engine) s->_engine->putError(id, copy);
    });
}

- (void)finishSending {
    _sending = NO;
    [self stopSendSpinner];
    _sendButton.enabledState = _hasRequest;
    _sendButton.icon = OS9SendImage(16);   // restore the send icon
    _cancelButton.enabledState = NO;
    [self relayout];
}

// Spinner in the Send button while sending: advance one spoke per tick (8 spokes -> ~smooth).
- (void)startSendSpinner {
    [_spinTimer invalidate];
    NSArray<NSImage *> *frames = OS9SpinnerFrames(16, 8);   // 8 prebuilt frames (cached) -> index, no allocation per tick
    _sendButton.icon = frames.firstObject;
    __weak MainWindowController *ws = self;
    __block NSUInteger idx = 0;
    _spinTimer = [NSTimer scheduledTimerWithTimeInterval:0.09 repeats:YES block:^(NSTimer *t) {
        MainWindowController *s = ws; if (!s) { [t invalidate]; return; }
        idx = (idx + 1) % frames.count;
        s->_sendButton.icon = frames[idx];
    }];
}
- (void)stopSendSpinner { [_spinTimer invalidate]; _spinTimer = nil; }

// Compute the display buffers (format JSON body/headers/cookie) — the HEAVY part, does NOT touch UI ->
// callable from a background thread. Depends only on params (r/type/prettyMode), reads no ivars.
- (NSArray<NSString *> *)computeResponseBuffersFor:(const core::ApiResponse &)r
                                              type:(core::RequestType)type
                                        prettyMode:(int)prettyMode {
    using namespace core;
    NSMutableArray<NSString *> *bufs = [NSMutableArray array];
    [bufs addObject:[self applyView:r.body mode:prettyMode]];   // body per Pretty/Raw/Encode/Decode
    if (type == RequestType::Http) {
        [bufs addObject:N(fieldcodec::formatJson(fieldcodec::keyValuesToJson(r.headers), true))];
        [bufs addObject:N(fieldcodec::formatJson(r.resolvedRequestDump, true))];
        NSMutableString *ck = [NSMutableString string];
        for (const auto &c : r.cookies)
            [ck appendFormat:@"%s=%s  (domain=%s path=%s expires=%s)\n", c.name.c_str(), c.value.c_str(),
                             c.domain.c_str(), c.path.c_str(), c.expires.c_str()];
        [bufs addObject:(ck.length ? ck : StrNoSetCookie)];
    } else {
        [bufs addObject:N(fieldcodec::formatJson(r.resolvedRequestDump, true))];
    }
    return bufs;
}

// Attach computed buffers to UI + select the remembered tab (LIGHT, runs on main thread).
- (void)applyResponseBuffers:(NSArray<NSString *> *)bufs {
    [_respBuffers removeAllObjects];
    [_respBuffers addObjectsFromArray:bufs];
    // Reapply the right pane's remembered tab (semantic key); no match -> first tab.
    NSInteger ri = [self tabIndexForKey:_rightPaneActiveTabKey inTitles:_respTabTitles];
    if (ri >= (NSInteger)_respBuffers.count) ri = 0;
    _activeRespTab = ri;
    _respText.string = _respBuffers.count ? _respBuffers[ri] : @"";
    [self highlightActiveTab:_respTabButtons active:ri];
}

// Synchronous: used for cheap re-renders (change view mode, change tab, stress).
- (void)rebuildResponseBuffers {
    [self applyResponseBuffers:[self computeResponseBuffersFor:_lastResp type:_model.type prettyMode:_prettyMode]];
}

// Asynchronous: format the response OFF the main thread then apply on main (U2 — large response won't freeze UI).
// Drop the result if a new request arrived (compare _currentHandle) -> avoid showing stale buffers.
- (void)rebuildResponseBuffersAsync {
    core::ApiResponse r = _lastResp;            // copy for safe background use (main won't mutate concurrently)
    core::RequestType type = _model.type;
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
    if (ms <= 0) return @"--:--:--.---";
    static NSDateFormatter *fmt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ fmt = [NSDateFormatter new]; fmt.dateFormat = @"HH:mm:ss.SSS"; });
    return [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:ms / 1000.0]];
}

// status | size | time | start - end. endMs = time response received; start = end - elapsed.
- (void)updateStatusFromResponse:(const core::ApiResponse &)r error:(BOOL)isErr endMs:(int64_t)endMs {
    NSString *code = r.statusCode ? [NSString stringWithFormat:@"%d", r.statusCode] : StrOK;
    NSString *size = (r.sizeBytes >= 1024) ? [NSString stringWithFormat:@"%.1fkb", r.sizeBytes / 1024.0]
                                           : [NSString stringWithFormat:@"%lldb", (long long)r.sizeBytes];
    int64_t startMs = (endMs > 0) ? endMs - (int64_t)r.elapsedMs : 0;   // derive the start mark
    NSString *range = [NSString stringWithFormat:@"%@ - %@",
                       [self clockFromEpochMs:startMs], [self clockFromEpochMs:endMs]];
    _statusLabel.stringValue = [NSString stringWithFormat:@"%@ | %@ | %ldms | %@", code, size, r.elapsedMs, range];
    _statusLabel.textColor = (r.statusCode >= 400) ? [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0]
                                                   : [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1.0];
}

@end
