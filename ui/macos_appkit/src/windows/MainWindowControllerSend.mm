#import "windows/MainWindowControllerPrivate.h"

@implementation MainWindowController (Send)

#pragma mark Send / Cancel

- (void)sendRequest:(id)sender {
    // Hot path: do ONLY what's needed to send (no env reads / aliasify / persistence here —
    // alias substitution is an import-time concern, §9.5). Keep this lean for performance.
    if (!_hasRequest || !_engine) return;
    // WebSocket is a session, not a one-shot send: the action toggles Connect / Send-frame (SPEC_websocket §6).
    if (_model.type == core::RequestType::WebSocket) { [self wsSendOrConnect]; return; }
    if (_sending) return;
    // Only HTTP has a URL query string to split; GraphQL's "Query" tab is the document, not params.
    if (_model.type == core::RequestType::Http) [self parseUrlQueryIntoQueryTab];
    if (![self syncModelFromEditors:NO]) return;

    // Route by interaction kind (SPEC_grpc_streaming §4). Methods that STREAM responses (server-streaming
    // + bidi) -> openStream(); unary and client-streaming (one response) -> send().
    core::InteractionKind kind = _engine->interactionOf(_model);
    BOOL streamsResponses = (kind == core::InteractionKind::ServerStream ||
                             kind == core::InteractionKind::BiDi);

    _sending = YES;
    [self startSendSpinner];           // spinning loading icon in place of the label
    _cancelButton.enabledState = YES;
    [self relayout];
    [self updateStatus:@""];

    if (streamsResponses) {
        _streaming = YES;
        _bridge->setStreamToken(++_streamToken);   // C2: stamp this stream so late callbacks from a prior one drop
        _streamHandle = _engine->openStream(_model, _bridge);
    } else {
        _streaming = NO;
        _currentHandle = _engine->send(_model, _bridge);
    }
}

- (void)cancelClicked:(id)sender {
    if (!_sending || !_engine) return;
    if (_model.type == core::RequestType::WebSocket) { _engine->closeSession(_wsSession, 1000, "bye"); return; }
    if (_streaming) _engine->cancelStream(_streamHandle);   // Stop -> ctx.TryCancel via CancelToken (§6)
    else _engine->cancel(_currentHandle);
}

// WebSocket action button: connect if idle; otherwise send the current Message editor as a frame (§6).
- (void)wsSendOrConnect {
    if (_sending && _wsSession.channel) {
        // Connected -> send the current Message tab content as a frame.
        [self stashActiveReqBuffer];
        std::string frame = _reqBuffers.count ? S(_reqBuffers[0]) : std::string();
        if (frame.empty()) { [self toastWarn:StrToastWsEmptyFrame]; return; }
        if (!_wsSession.channel->sendText(frame)) [self toastWarn:StrToastWsQueueFull];
        return;
    }
    if (_sending) return;   // connecting in progress
    if (![self syncModelFromEditors:NO]) return;
    _sending = YES;
    _streaming = YES;       // reuse the streaming UI state (Cancel = Disconnect, log render)
    [self startSendSpinner];
    _cancelButton.enabledState = YES;
    [self relayout];
    [self updateStatus:@""];
    _bridge->setStreamToken(++_streamToken);   // C2: stamp this session
    _wsSession = _engine->openSession(_model, _bridge);
}

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
    // gRPC send failed -> the RPC list may be stale (server down/changed). Invalidate so the next
    // dropdown open re-fetches it (requirement: re-fetch starting from the failed send).
    if (_model.type == core::RequestType::Grpc) _grpcMethodsFetched = NO;
    [self finishSending];
    [self displayErrorKind:err.kind message:N(err.message)];
    [self toastWarn:[NSString stringWithFormat:@"%@: %@", N(core::toString(err.kind)), N(err.message)]];
    [self cacheErrorAsync:err forId:_currentId];       // cache errors too, to restore the exact state (§7)
}

#pragma mark Streaming (SPEC_grpc_streaming §7)

// '[' — reset the response pane into streaming-write mode. (transport arg is display/telemetry only — INV-1)
- (void)onStreamOpenTransport:(int)transport token:(uint64_t)token {
    (void)transport;
    if (token != _streamToken) return;   // C2: a stale stream's open -> ignore
    _hasResp = NO;
    _streamEvents = 0;
    // Show the streaming text in the body tab (tab 0) and select it.
    _activeRespTab = 0;
    [self highlightActiveTab:_respTabButtons active:0];
    [_respText beginStreaming];   // seeds "[\n]" -> the response is a valid JSON array from the start
    _statusLabel.stringValue = [NSString stringWithFormat:StrFmtStreamReceived, 0ULL];
    _statusLabel.textColor = [NSColor blackColor];
}

// Coalesced append (already comma-joined per Appendix A). Update the live counter.
- (void)onStreamChunk:(NSString *)chunk events:(uint64_t)totalEvents token:(uint64_t)token {
    if (token != _streamToken || !_streaming || !chunk.length) return;   // C2: drop a prior stream's chunk
    [_respText insertStreamChunk:chunk];   // inserted before the trailing "]" -> stays valid live (doc is the array, H3)
    _streamEvents = totalEvents;
    _statusLabel.stringValue = [NSString stringWithFormat:StrFmtStreamReceived, (unsigned long long)totalEvents];
}

// ']' + finalize status; swap the live text for the normal formatted buffers; cache the array (§8).
- (void)onStreamClose:(core::StreamStatus)status code:(int)code message:(NSString *)message
               events:(uint64_t)events elapsedMs:(long long)elapsedMs truncated:(BOOL)truncated
                token:(uint64_t)token {
    if (token != _streamToken) return;   // C2: a stale stream's close must not touch the current request's panes
    // The pane already shows a closed, valid array (seeded "[\n]"; each event was inserted before the
    // trailing "]"). The Scintilla document IS the assembled array — read it back here instead of keeping
    // a second NSMutableString copy growing alongside it during the stream (H3).
    [_respText endStreamingValid:YES];

    _streaming = NO;
    _wsSession = core::SessionHandle{};   // release the WS channel/session (no-op for gRPC streams)
    [self finishSending];

    // Build a neutral ApiResponse from the assembled array so the normal tab/cache pipeline takes over.
    core::ApiResponse resp;
    resp.statusCode = 0;
    resp.statusText = "OK";
    resp.body = S(_respText.string);   // the live doc already holds the valid [ … ] array (H3)
    resp.elapsedMs = (long)elapsedMs;
    resp.sizeBytes = (std::int64_t)resp.body.size();
    resp.wasStreamed = true;
    resp.eventCount = events;
    resp.partial = (status != core::StreamStatus::Ok);

    _lastResp = resp;
    _hasResp = YES;
    [self rebuildResponseBuffers];   // reformat the captured array into the body/Request tabs

    // Status line, fields separated by '|'. Size is shown for Ok AND Cancelled — so a stream stopped
    // mid-way still reports how much was received.
    int64_t sz = (int64_t)resp.body.size();
    NSString *sizeStr = (sz >= 1024) ? [NSString stringWithFormat:@"%.1fkb", sz / 1024.0]
                                     : [NSString stringWithFormat:@"%lldb", (long long)sz];
    NSString *line;
    NSColor *color;
    NSString *trunc = truncated ? StrStreamTruncated : @"";
    if (status == core::StreamStatus::Ok) {
        line = [NSString stringWithFormat:StrFmtStreamOk, trunc, sizeStr, (unsigned long long)events, elapsedMs];
        color = [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1.0];
    } else if (status == core::StreamStatus::Cancelled) {
        line = [NSString stringWithFormat:StrFmtStreamCancelled, StrStatusCancelled, sizeStr,
                                          (unsigned long long)events];
        color = [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0];
    } else {
        NSString *kind = (status == core::StreamStatus::Timeout) ? StrStreamKindTimeout : StrStreamKindError;
        line = [NSString stringWithFormat:StrFmtStreamError, kind, code, message ?: @""];
        color = [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0];
        if (_model.type == core::RequestType::Grpc) _grpcMethodsFetched = NO;   // re-fetch RPCs next open
    }
    _statusLabel.stringValue = line;
    _statusLabel.textColor = color;

    // Stream responses (gRPC stream / WS / SSE) are NOT cached — they are live, open-ended and can be
    // huge; persisting them would bloat the cache (and a partial/cancelled capture is rarely useful).
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
    _respText.string = _respBuffers.count ? _respBuffers[0] : @"";   // L1: guard empty buffers
    [self highlightActiveTab:_respTabButtons active:0];
}

// Write cache on a BACKGROUND thread (§6: put async, don't block UI). Engine is thread-safe.
- (void)cacheResponseAsync:(const core::ApiResponse &)resp forId:(const std::string &)reqId {
    if (reqId.empty()) return;
    __block core::ApiResponse copy = resp;   // one copy to own it across the async hop; moved into cache below (M3)
    std::string id = reqId;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        MainWindowController *s = ws;
        if (s && s->_engine) s->_engine->putResponse(id, std::move(copy));   // move overload, no second copy
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
    // L2: NSDateFormatter is NOT thread-safe and this instance is shared. MAIN-THREAD ONLY — callers run on
    // the main queue (status updates). Do NOT call from computeResponseBuffersFor: or any background worker.
    NSAssert([NSThread isMainThread], @"clockFromEpochMs: must be called on the main thread (shared formatter)");
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
