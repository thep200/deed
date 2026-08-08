#import "windows/MainWindowControllerPrivate.h"

@implementation MainWindowController (Send)

#pragma mark Send / Cancel

// Pull the active env's {{vars}} into the send scope. Lazy encryption check: a var still holding
// ciphertext here means the configured key can't read it -> warn once per send, then send anyway.
- (void)refreshVarsForSend {
    _apiClient->refreshVariableScope();
    if (_apiClient->hasUnreadableVars()) [self toastWarn:StrToastEncKeyInvalid];
}

- (void)sendRequest:(id)sender {
    // Hot path: do ONLY what's needed to send (no env reads / aliasify / persistence here —
    // alias substitution is an import-time concern). Keep this lean for performance.
    if (!_hasRequest || !_apiClient) return;
    // WebSocket is a session, not a one-shot send: the action toggles Connect / Send-frame.
    if ([self requestType] == core::RequestType::WebSocket) { [self wsSendOrConnect]; return; }
    if (_sending) return;
    // Only HTTP has a URL query string to split; GraphQL's "Query" tab is the document, not params.
    if ([self requestType] == core::RequestType::Http) [self parseUrlQueryIntoQueryTab];
    if (![self syncModelFromEditors:NO]) return;

    // Route by interaction kind: server-streaming + bidi STREAM responses -> streamViaApiClient;
    // unary and client-streaming (one response) -> sendViaApiClient.
    core::InteractionKind kind =
        _model ? _apiClient->interactionOf(*_model) : core::InteractionKind::Unary;
    BOOL streamsResponses = (kind == core::InteractionKind::ServerStream ||
                             kind == core::InteractionKind::BiDi);

    _sending = YES;
    [self startSendSpinner];           // spinning loading icon in place of the label
    _cancelButton.enabledState = YES;
    [self relayout];
    [self updateStatus:@""];

    if (streamsResponses) {
        _streaming = YES;
        [self streamViaApiClient];
    } else {
        _streaming = NO;
        [self sendViaApiClient];
    }
    [self beginRequestTiming];   // live elapsed (+ size while streaming) on the status line
}

// Unary send through IApiClient. A UiObserver translates domain ResponseEvents back into the
// onCoreResponse/onCoreError handlers (keyed by a synthesized handle so the stale-callback guard still works).
- (void)sendViaApiClient {
    [self refreshVarsForSend];   // {{vars}} resolve against the current active environment

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
    [self refreshVarsForSend];   // {{vars}} resolve against the current active environment

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

// Cancel outranks everything: it must stop the connection even when the request is wedged in a syscall,
// the exec handle is stale, or core never reports a terminal event. Three stages, escalating:
//   1. cancel(exec)  — trips the saga token; every sender's abort hook kills its live socket/call.
//   2. cancelAll()   — no handle yet / wrong saga -> kill whatever is in flight.
//   3. force-settle  — bump the guards so late events drop, then release the UI.
- (void)cancelClicked:(id)sender {
    if (!_sending || !_apiClient) return;
    if (!_apiExec.empty()) {
        // WS: graceful disconnect first (a clean close is the normal end of a session, not a failure).
        if ([self requestType] == core::RequestType::WebSocket) _apiClient->closeStream(_apiExec, 1000, "bye");
        _apiClient->cancel(_apiExec);
        _cancelStage = 1;
    } else {
        _apiClient->cancelAll();   // send still registering -> no handle to aim at
        _cancelStage = 2;
    }
    [self armCancelWatchdog];
}

// Re-fires every 1.2s while the request refuses to settle, escalating one stage each time.
- (void)armCancelWatchdog {
    [_cancelWatchdog invalidate];
    __weak MainWindowController *ws = self;
    _cancelWatchdog = [NSTimer scheduledTimerWithTimeInterval:1.2 repeats:YES block:^(NSTimer *t) {
        MainWindowController *s = ws;
        if (!s) { [t invalidate]; return; }
        if (!s->_sending) { [s stopCancelWatchdog]; return; }
        if (s->_cancelStage < 2) {
            s->_cancelStage = 2;
            if (s->_apiClient) s->_apiClient->cancelAll();
            return;
        }
        // Core is wedged below the cancel points. Cut the UI loose; the guards drop any late event.
        [s stopCancelWatchdog];
        s->_currentHandle = 0;
        s->_streamToken++;
        s->_streaming = NO;
        [s finishSending];
        [s displayErrorKind:core::domain::ErrorKind::Cancelled message:StrStatusCancelled
                  elapsedMs:[s measuredElapsedMs]];
        [s toastWarn:StrToastCancelForced];
    }];
}

- (void)stopCancelWatchdog {
    [_cancelWatchdog invalidate];
    _cancelWatchdog = nil;
    _cancelStage = 0;
}

- (void)wsSendOrConnect {
    [self wsViaApiClient];
}

// WebSocket via IApiClient: connect if idle, else push the Message editor as a frame through the
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

    [self refreshVarsForSend];   // {{vars}} resolve against the current active environment
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
    [self rebuildResponseBuffersAsync];   // format off the main thread -> large response won't freeze UI
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
    // GraphQL send failed -> the cached schema may be stale for the same reason (click-triggered re-fetch).
    if ([self requestType] == core::RequestType::GraphQl) [self invalidateGqlSchema];
    [self finishSending];
    [self displayErrorKind:err.kind message:N(err.message) elapsedMs:[self measuredElapsedMs]];
    [self toastWarn:[NSString stringWithFormat:@"%@: %@", N(core::domain::toString(err.kind)), N(err.message)]];
    [self cacheErrorAsync:err forId:_currentId];       // cache errors too, to restore the exact state
}

#pragma mark Streaming (SPEC_grpc_streaming §7)

// '[' — reset the response pane into streaming-write mode. (transport arg is display/telemetry only)
- (void)onStreamOpenTransport:(int)transport token:(uint64_t)token {
    (void)transport;
    if (token != _streamToken) return;   // a stale stream's open -> ignore
    _hasResp = NO;
    _streamEvents = 0;
    _streamBytes = 0;   // reset live size counter for this stream
    // Show the streaming text in the body tab (tab 0) and select it.
    _activeRespTab = 0;
    [self highlightActiveTab:_respTabButtons active:0];
    [_respText setLanguage:SciLanguageJson]; // stream log is a JSON array, whatever was shown before
    [_respText beginStreaming];   // seeds "[\n]" -> the response is a valid JSON array from the start
    [self liveTick];              // paint the live status (elapsed | size | events) immediately
}

// Coalesced append (already comma-joined members). Update the live counters;
// the live timer (liveTick) renders the status line so frequent chunks don't each touch the label (perf).
- (void)onStreamChunk:(NSString *)chunk events:(uint64_t)totalEvents token:(uint64_t)token {
    if (token != _streamToken || !_streaming || !chunk.length) return;   // drop a prior stream's chunk
    [_respText insertStreamChunk:chunk];   // inserted before the trailing "]" -> stays valid live
    _streamEvents = totalEvents;
    _streamBytes += (int64_t)[chunk lengthOfBytesUsingEncoding:NSUTF8StringEncoding];   // running size (live)
}

// ']' + finalize status; swap the live text for the normal formatted buffers; cache the array.
- (void)onStreamClose:(core::StreamStatus)status code:(int)code message:(NSString *)message
               events:(uint64_t)events elapsedMs:(long long)elapsedMs truncated:(BOOL)truncated
                token:(uint64_t)token {
    if (token != _streamToken) return;   // a stale stream's close must not touch the current request's panes
    // The pane already shows a closed, valid array (seeded "[\n]"; each event was inserted before the
    // trailing "]"). The Scintilla document IS the assembled array — read it back here instead of keeping
    // a second NSMutableString copy growing alongside it during the stream.
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
    resp.body = S(_respText.string);   // the live doc already holds the valid [ … ] array
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
        color = [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1.0];
    } else if (effStatus == core::StreamStatus::Cancelled) {
        line = [NSString stringWithFormat:StrFmtStreamCancelled, StrStatusCancelled, sizeStr,
                                          (unsigned long long)events, [self elapsedText:effElapsedMs]];
        color = [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0];
    } else {
        NSString *kind = (effStatus == core::StreamStatus::Timeout) ? StrStreamKindTimeout : StrStreamKindError;
        line = [NSString stringWithFormat:StrFmtStreamError, kind, code, message ?: @""];
        color = [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0];
        if ([self requestType] == core::RequestType::Grpc) _grpcMethodsFetched = NO;   // re-fetch RPCs next open
        if ([self requestType] == core::RequestType::GraphQl) [self invalidateGqlSchema]; // schema stale too
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
    _statusLabel.textColor = [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0];
    _hasResp = NO;
    [_respBuffers removeAllObjects];
    for (NSUInteger i = 0; i < _respTabTitles.count; i++) [_respBuffers addObject:@""];
    if (_respBuffers.count) _respBuffers[0] = [NSString stringWithFormat:@"[%@] %@", k, msg ?: @""];
    _activeRespTab = 0;
    _respText.string = _respBuffers.count ? _respBuffers[0] : @"";   // guard empty buffers
    [self highlightActiveTab:_respTabButtons active:0];
}

// Write cache on a BACKGROUND thread (async put, don't block UI); the cache is thread-safe.
- (void)cacheResponseAsync:(const core::domain::ApiResponse &)resp forId:(const std::string &)reqId {
    if (reqId.empty()) return;
    __block core::domain::ApiResponse copy = resp;   // own a copy across the background hop
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
    [self stopCancelWatchdog];
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

@end
