#import "windows/MainWindowControllerPrivate.h"

@implementation MainWindowController (Send)

#pragma mark Send / Cancel

- (void)sendRequest:(id)sender {
    // Hot path: do ONLY what's needed to send (no env reads / aliasify / persistence here —
    // alias substitution is an import-time concern, §9.5). Keep this lean for performance.
    if (!_hasRequest || !_apiClient) return;
    // WebSocket is a session, not a one-shot send: the action toggles Connect / Send-frame (SPEC_websocket §6).
    if ([self requestType] == core::RequestType::WebSocket) { [self wsSendOrConnect]; return; }
    if (_sending) return;
    // Only HTTP has a URL query string to split; GraphQL's "Query" tab is the document, not params.
    if ([self requestType] == core::RequestType::Http) [self parseUrlQueryIntoQueryTab];
    if (![self syncModelFromEditors:NO]) return;

    // Route by interaction kind (SPEC_grpc_streaming §4). Methods that STREAM responses (server-streaming
    // + bidi) -> openStream(); unary and client-streaming (one response) -> send().
    core::InteractionKind kind =
        _model ? _apiClient->interactionOf(*_model) : core::InteractionKind::Unary;
    BOOL streamsResponses = (kind == core::InteractionKind::ServerStream ||
                             kind == core::InteractionKind::BiDi);

    _sending = YES;
    [self startSendSpinner];           // spinning loading icon in place of the label
    _cancelButton.enabledState = YES;
    [self relayout];
    [self updateStatus:@""];

    // REFACTOR_SPEC P6: ALL sends go through the IApiClient stack (orchestrator/saga/domain senders).
    // Server-stream (gRPC server-streaming / HTTP SSE) -> streamViaApiClient; unary HTTP/gRPC/GraphQL ->
    // sendViaApiClient. WebSocket is handled above (wsSendOrConnect). No legacy Engine send path remains.
    if (streamsResponses) {
        _streaming = YES;
        [self streamViaApiClient];
    } else {
        _streaming = NO;
        [self sendViaApiClient];
    }
    [self beginRequestTiming];   // live elapsed (+ size while streaming) on the status line
}

// New send path (REFACTOR_SPEC P6): convert the legacy model to domain, feed the active env vars, and send
// through IApiClient. A UiObserver translates the domain ResponseEvents back into the existing
// onCoreResponse/onCoreError handlers (keyed by a synthesized handle so the stale-callback guard still works).
- (void)sendViaApiClient {
    _apiClient->refreshVariableScope();   // {{vars}} resolve against the current active environment

    uint64_t h = ++_apiHandleCounter;
    _currentHandle = h;                          // CoreResponseSink guards on this handle
    if (!_model) {
        [self onCoreError:h error:core::domain::ApiError{core::domain::ErrorKind::Internal,
                                                         "no open request", std::nullopt}];
        return;
    }
    _apiObserver = std::make_shared<UiObserver>(self, h);
    auto r = _apiClient->send(*_model, _apiObserver);
    if (r.isOk()) {
        _apiExec = r.value();
    } else {
        [self onCoreError:h error:core::domain::ApiError{core::domain::ErrorKind::Internal,
                                                         r.error().message, std::nullopt}];
    }
}

// Server-stream (gRPC server-streaming / HTTP SSE) via IApiClient. A streaming UiObserver maps the domain
// ResponseEvents onto the existing onStreamOpenTransport/onStreamChunk/onStreamClose handlers.
- (void)streamViaApiClient {
    _apiClient->refreshVariableScope();   // {{vars}} resolve against the current active environment

    uint64_t token = ++_streamToken;
    int transport = ([self requestType] == core::RequestType::Http) ? 1 /*sse*/ : 0 /*grpc*/;
    if (!_model) {
        [self onStreamOpenTransport:transport token:token];
        [self onStreamClose:core::StreamStatus::Error code:0 message:@"no open request"
                     events:0 elapsedMs:0 truncated:NO token:token];
        return;
    }
    _apiObserver = std::make_shared<UiObserver>(self, token, transport);
    auto r = _apiClient->send(*_model, _apiObserver);
    if (r.isOk()) {
        _apiExec = r.value();
    } else {
        [self onStreamOpenTransport:transport token:token];
        [self onStreamClose:core::StreamStatus::Error code:0 message:N(r.error().message)
                     events:0 elapsedMs:0 truncated:NO token:token];
    }
}

- (void)cancelClicked:(id)sender {
    if (!_sending || !_apiClient || _apiExec.empty()) return;
    if ([self requestType] == core::RequestType::WebSocket) {
        _apiClient->closeStream(_apiExec, 1000, "bye");        // WS disconnect
        return;
    }
    _apiClient->cancel(_apiExec);                              // unary OR server-stream via IApiClient
}

// WebSocket action button (§6): connect if idle, else send the Message editor as a frame.
// REFACTOR_SPEC P6: WebSocket runs entirely through IApiClient (connect/send-frame/disconnect).
- (void)wsSendOrConnect {
    [self wsViaApiClient];
}

// WebSocket via IApiClient (§6): connect if idle, else push the Message editor as a frame through the
// open session. Inbound frames + close arrive via the streaming UiObserver -> onStream* handlers.
- (void)wsViaApiClient {
    if (_apiWsActive && !_apiExec.empty()) {
        [self stashActiveReqBuffer];
        std::string frame = _reqBuffers.count ? S(_reqBuffers[0]) : std::string();
        if (frame.empty()) { [self toastWarn:StrToastWsEmptyFrame]; return; }
        auto st = _apiClient->sendStreamMessage(
            _apiExec, core::domain::WsMessage{core::domain::WsSendKind::Text, frame});
        if (!st.isOk()) [self toastWarn:StrToastWsQueueFull];
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

    _apiClient->refreshVariableScope();   // {{vars}} resolve against the current active environment
    if (!_model) { _sending = NO; [self toastWarn:@"no open request"]; [self relayout]; return; }
    uint64_t token = ++_streamToken;
    _apiObserver = std::make_shared<UiObserver>(self, token, 2 /*ws*/);
    auto r = _apiClient->send(*_model, _apiObserver);
    if (r.isOk()) { _apiExec = r.value(); _apiWsActive = YES; }
    else { _sending = NO; [self toastWarn:N(r.error().message)]; [self relayout]; return; }
    [self beginRequestTiming];
}

- (void)onCoreResponse:(uint64_t)handle response:(const core::domain::ApiResponse &)resp {
    if (handle != _currentHandle) return;
    NSLog(@"[smoke] onCoreResponse status=%d bytes=%lld", resp.statusCode, (long long)resp.body.size());
    [self finishSending];
    _lastResp = resp; _hasResp = YES;
    [self rebuildResponseBuffersAsync];   // format off the main thread -> large response won't freeze UI (U2)
    int64_t endMs = (int64_t)([[NSDate date] timeIntervalSince1970] * 1000.0);  // end mark = time received
    [self updateStatusFromResponse:resp error:NO endMs:endMs];
    [self cacheResponseAsync:resp forId:_currentId];   // store cache (background) — keyed by id
}

- (void)onCoreError:(uint64_t)handle error:(const core::domain::ApiError &)err {
    if (handle != _currentHandle) return;
    NSLog(@"[smoke] onCoreError kind=%s msg=%s", core::domain::toString(err.kind).c_str(), err.message.c_str());
    // gRPC send failed -> the RPC list may be stale (server down/changed). Invalidate so the next
    // dropdown open re-fetches it (requirement: re-fetch starting from the failed send).
    if ([self requestType] == core::RequestType::Grpc) _grpcMethodsFetched = NO;
    [self finishSending];
    [self displayErrorKind:err.kind message:N(err.message) elapsedMs:[self measuredElapsedMs]];
    [self toastWarn:[NSString stringWithFormat:@"%@: %@", N(core::domain::toString(err.kind)), N(err.message)]];
    [self cacheErrorAsync:err forId:_currentId];       // cache errors too, to restore the exact state (§7)
}

#pragma mark Streaming (SPEC_grpc_streaming §7)

// '[' — reset the response pane into streaming-write mode. (transport arg is display/telemetry only — INV-1)
- (void)onStreamOpenTransport:(int)transport token:(uint64_t)token {
    (void)transport;
    if (token != _streamToken) return;   // C2: a stale stream's open -> ignore
    _hasResp = NO;
    _streamEvents = 0;
    _streamBytes = 0;   // reset live size counter for this stream
    // Show the streaming text in the body tab (tab 0) and select it.
    _activeRespTab = 0;
    [self highlightActiveTab:_respTabButtons active:0];
    [_respText beginStreaming];   // seeds "[\n]" -> the response is a valid JSON array from the start
    [self liveTick];              // paint the live status (elapsed | size | events) immediately
}

// Coalesced append (already comma-joined "ev_N" members per Appendix A). Update the live counters;
// the live timer (liveTick) renders the status line so frequent chunks don't each touch the label (perf).
- (void)onStreamChunk:(NSString *)chunk events:(uint64_t)totalEvents token:(uint64_t)token {
    if (token != _streamToken || !_streaming || !chunk.length) return;   // C2: drop a prior stream's chunk
    [_respText insertStreamChunk:chunk];   // inserted before the trailing "]" -> stays valid live (doc is the array, H3)
    _streamEvents = totalEvents;
    _streamBytes += (int64_t)[chunk lengthOfBytesUsingEncoding:NSUTF8StringEncoding];   // running size (live)
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
    [self finishSending];

    // SSE (an HTTP stream) is open-ended: pressing Cancel/Stop is the NORMAL way to end it, not a failure.
    // Report it as Ok so the live status matches the cached restore (which reopens as Ok) — the status no
    // longer flips Cancelled->OK on re-view. gRPC/WebSocket cancel still reports Cancelled.
    core::StreamStatus effStatus = ([self requestType] == core::RequestType::Http &&
                                    status == core::StreamStatus::Cancelled)
                                       ? core::StreamStatus::Ok : status;

    // A user-pressed Cancel/Disconnect settles the stream with elapsedMs==0 (the sender reports no duration);
    // fall back to the UI's measured elapsed so the status shows the REAL time instead of 0ms.
    long long effElapsedMs = (elapsedMs > 0) ? elapsedMs : [self measuredElapsedMs];

    // Build a neutral domain ApiResponse from the assembled array so the normal tab/cache pipeline takes over.
    core::domain::ApiResponse resp;
    resp.statusCode = 0;
    resp.body = S(_respText.string);   // the live doc already holds the valid [ … ] array (H3)
    resp.elapsed = std::chrono::milliseconds(effElapsedMs);

    _lastResp = resp;
    _hasResp = YES;
    [self rebuildResponseBuffers];   // reformat the captured array into the body/Request tabs

    // Status line, fields separated by '|'. Size is shown for Ok AND Cancelled — so a stream stopped
    // mid-way still reports how much was received.
    int64_t sz = (int64_t)resp.body.size();
    NSString *sizeStr = [self humanSize:sz];
    NSString *line;
    NSColor *color;
    NSString *trunc = truncated ? StrStreamTruncated : @"";
    if (effStatus == core::StreamStatus::Ok) {
        line = [NSString stringWithFormat:StrFmtStreamOk, trunc, sizeStr, (unsigned long long)events,
                                          [self elapsedText:effElapsedMs]];
        color = [OS9Theme statusOk];
    } else if (effStatus == core::StreamStatus::Cancelled) {
        line = [NSString stringWithFormat:StrFmtStreamCancelled, StrStatusCancelled, sizeStr,
                                          (unsigned long long)events, [self elapsedText:effElapsedMs]];
        color = [OS9Theme statusError];
    } else {
        NSString *kind = (effStatus == core::StreamStatus::Timeout) ? StrStreamKindTimeout : StrStreamKindError;
        line = [NSString stringWithFormat:StrFmtStreamError, kind, code, message ?: @""];
        color = [OS9Theme statusError];
        if ([self requestType] == core::RequestType::Grpc) _grpcMethodsFetched = NO;   // re-fetch RPCs next open
    }
    _statusLabel.stringValue = line;
    _statusLabel.textColor = color;

    // Cache the assembled stream (the captured array) keyed by request id, exactly like a unary
    // response — so re-opening the request restores what was received (incl. partial/cancelled).
    [self cacheResponseAsync:resp forId:_currentId];
}

// Display the error state in the response pane (shared by new errors and cached errors).
- (void)displayErrorKind:(core::domain::ErrorKind)kind message:(NSString *)msg elapsedMs:(long long)elapsedMs {
    NSString *k = N(core::domain::toString(kind));
    NSString *statusText = k;                                       // drop the ✕ before the error name
    if (kind == core::domain::ErrorKind::Cancelled) statusText = StrStatusCancelled;
    else if (kind == core::domain::ErrorKind::Network) statusText = StrStatusNetworkError;   // network error -> report clearly
    // Show how long elapsed before the error/cancel (e.g. "Cancelled | 1.20s"). elapsedMs<=0 (cached errors,
    // which carry no live duration) -> status only, no time.
    if (elapsedMs > 0) statusText = [NSString stringWithFormat:@"%@ | %@", statusText, [self elapsedText:elapsedMs]];
    _statusLabel.stringValue = statusText;
    _statusLabel.textColor = [OS9Theme statusError];
    _hasResp = NO;
    [_respBuffers removeAllObjects];
    for (NSUInteger i = 0; i < _respTabTitles.count; i++) [_respBuffers addObject:@""];
    if (_respBuffers.count) _respBuffers[0] = [NSString stringWithFormat:@"[%@] %@", k, msg ?: @""];
    _activeRespTab = 0;
    _respText.string = _respBuffers.count ? _respBuffers[0] : @"";   // L1: guard empty buffers
    [self highlightActiveTab:_respTabButtons active:0];
}

// Write cache on a BACKGROUND thread (§6: put async, don't block UI). Engine is thread-safe.
- (void)cacheResponseAsync:(const core::domain::ApiResponse &)resp forId:(const std::string &)reqId {
    if (reqId.empty()) return;
    __block core::domain::ApiResponse copy = resp;   // cache speaks domain now (REFACTOR_SPEC D)
    std::string id = reqId;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        MainWindowController *s = ws;
        if (s && s->_apiClient) s->_apiClient->cache().putResponse(id, std::move(copy)); // move, no 2nd copy
    });
}
- (void)cacheErrorAsync:(const core::domain::ApiError &)err forId:(const std::string &)reqId {
    if (reqId.empty()) return;
    core::domain::ApiError copy = err;
    std::string id = reqId;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        MainWindowController *s = ws;
        if (s && s->_apiClient) s->_apiClient->cache().putError(id, copy);
    });
}

- (void)finishSending {
    _sending = NO;
    _apiExec = core::domain::RequestExecutionId("");   // new-path send settled -> no stale cancel/push target
    _apiWsActive = NO;
    [self stopLiveTimer];   // freeze the live ticker; the final status line is set by the caller next
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
    _statusLabel.textColor = [OS9Theme textPrimary];
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
    using namespace core;
    NSMutableArray<NSString *> *bufs = [NSMutableArray array];
    [bufs addObject:[self applyView:r.body mode:prettyMode]];   // body per Pretty/Raw/Encode/Decode
    // The "Request" tab once showed the response's resolvedRequestDump; the domain ApiResponse doesn't
    // carry it (the resolved request is derivable from the model). Empty until that's wired UI-side.
    if (type == RequestType::Http) {
        [bufs addObject:N(core::serial::responseHeadersToJson(r.headers))];
        [bufs addObject:@""];   // Request tab (resolved request) — see note above
        NSMutableString *ck = [NSMutableString string];
        for (const auto &c : r.cookies)
            [ck appendFormat:@"%s=%s  (domain=%s path=%s expires=%s)\n", c.name.c_str(), c.value.c_str(),
                             c.domain.c_str(), c.path.c_str(), c.expires.c_str()];
        [bufs addObject:(ck.length ? ck : StrNoSetCookie)];
    } else if (type != RequestType::Kafka) {
        [bufs addObject:@""];   // Request tab (resolved request) — see note above
    }
    // Kafka has no Request tab (removed per product decision) -> body buffer only, no trailing empty slot.
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
    [self applyResponseBuffers:[self computeResponseBuffersFor:_lastResp type:[self requestType] prettyMode:_prettyMode]];
}

// Asynchronous: format the response OFF the main thread then apply on main (U2 — large response won't freeze UI).
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
    _statusLabel.textColor = [OS9Theme textPrimary];
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
- (void)updateStatusFromResponse:(const core::domain::ApiResponse &)r error:(BOOL)isErr endMs:(int64_t)endMs {
    long elapsedMs = (long)r.elapsed.count();
    NSString *code = r.statusCode ? [NSString stringWithFormat:@"%d", r.statusCode] : StrOK;
    NSString *size = [self humanSize:(int64_t)r.body.size()];
    int64_t startMs = (endMs > 0) ? endMs - (int64_t)elapsedMs : 0;   // derive the start mark
    NSString *range = [NSString stringWithFormat:@"%@ - %@",
                       [self clockFromEpochMs:startMs], [self clockFromEpochMs:endMs]];
    _statusLabel.stringValue = [NSString stringWithFormat:@"%@ | %@ | %@ | %@", code, size,
                                                          [self elapsedText:elapsedMs], range];
    _statusLabel.textColor = (r.statusCode >= 400) ? [OS9Theme statusError]
                                                   : [OS9Theme statusOk];
}

@end
