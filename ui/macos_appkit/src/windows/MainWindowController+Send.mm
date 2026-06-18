#import "windows/MainWindowController+Private.h"

@implementation MainWindowController (Send)

#pragma mark Send / Cancel

- (void)sendRequest:(id)sender {
    if (!_hasRequest || !_engine || _sending) return;
    [self parseUrlQueryIntoQueryTab];   // user gõ query vào URL -> tách vào tab Query trước khi sync
    if (![self syncModelFromEditors:NO]) return;
    if (_model.type == core::RequestType::Grpc && _model.grpc.methodType != "unary") {
        [self toastWarn:@"POC supports unary gRPC only"]; return;
    }
    _sending = YES;
    [self startSendSpinner];           // icon loading quay thay cho label
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
    [self rebuildResponseBuffers];
    int64_t endMs = (int64_t)([[NSDate date] timeIntervalSince1970] * 1000.0);  // mốc kết thúc = lúc nhận
    [self updateStatusFromResponse:resp error:NO endMs:endMs];
    [self cacheResponseAsync:resp forId:_currentId];   // lưu cache (nền) — khoá theo id
}

- (void)onCoreError:(uint64_t)handle error:(const core::ApiError &)err {
    if (handle != _currentHandle) return;
    NSLog(@"[smoke] onCoreError kind=%s msg=%s", core::toString(err.kind).c_str(), err.message.c_str());
    [self finishSending];
    [self displayErrorKind:err.kind message:N(err.message)];
    [self toastWarn:[NSString stringWithFormat:@"%@: %@", N(core::toString(err.kind)), N(err.message)]];
    [self cacheErrorAsync:err forId:_currentId];       // cache cả lỗi để hiện lại đúng trạng thái (§7)
}

// Hiển thị trạng thái lỗi vào pane response (dùng chung cho lỗi mới lẫn lỗi từ cache).
- (void)displayErrorKind:(core::ErrorKind)kind message:(NSString *)msg {
    NSString *k = N(core::toString(kind));
    _statusLabel.stringValue = (kind == core::ErrorKind::Cancelled) ? @"Cancelled" : [NSString stringWithFormat:@"✕ %@", k];
    _statusLabel.textColor = [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0];
    _hasResp = NO;
    [_respBuffers removeAllObjects];
    for (NSUInteger i = 0; i < _respTabTitles.count; i++) [_respBuffers addObject:@""];
    if (_respBuffers.count) _respBuffers[0] = [NSString stringWithFormat:@"[%@] %@", k, msg ?: @""];
    _activeRespTab = 0;
    _respText.string = _respBuffers[0];
    [self highlightActiveTab:_respTabButtons active:0];
}

// Ghi cache ở thread NỀN (§6: put async, không block UI). Engine thread-safe.
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
    _sendButton.icon = OS9SendImage(16);   // trả lại icon send
    _cancelButton.enabledState = NO;
    [self relayout];
}

// Spinner trong nút Send khi đang gửi: quay 1 nan mỗi tick (8 nan -> ~mượt).
- (void)startSendSpinner {
    _spinPhase = 0;
    [_spinTimer invalidate];
    _sendButton.icon = OS9SpinnerImage(16, 0);
    __weak MainWindowController *ws = self;
    _spinTimer = [NSTimer scheduledTimerWithTimeInterval:0.09 repeats:YES block:^(NSTimer *t) {
        MainWindowController *s = ws; if (!s) { [t invalidate]; return; }
        s->_spinPhase += 1.0 / 8.0;
        if (s->_spinPhase >= 1.0) s->_spinPhase -= 1.0;
        s->_sendButton.icon = OS9SpinnerImage(16, s->_spinPhase);
    }];
}
- (void)stopSendSpinner { [_spinTimer invalidate]; _spinTimer = nil; }

- (void)rebuildResponseBuffers {
    [_respBuffers removeAllObjects];
    using namespace core;
    const ApiResponse &r = _lastResp;
    if (_model.type == RequestType::Http) {
        [_respBuffers addObject:[self applyView:r.body]]; // body theo chế độ Pretty/Raw/Encode/Decode
        [_respBuffers addObject:N(fieldcodec::formatJson(fieldcodec::keyValuesToJson(r.headers), true))];
        [_respBuffers addObject:N(fieldcodec::formatJson(r.resolvedRequestDump, true))];
        NSMutableString *ck = [NSMutableString string];
        for (const auto &c : r.cookies)
            [ck appendFormat:@"%s=%s  (domain=%s path=%s expires=%s)\n", c.name.c_str(), c.value.c_str(),
                             c.domain.c_str(), c.path.c_str(), c.expires.c_str()];
        [_respBuffers addObject:(ck.length ? ck : @"(no Set-Cookie)")];
    } else {
        [_respBuffers addObject:[self applyView:r.body]];
        [_respBuffers addObject:N(fieldcodec::formatJson(r.resolvedRequestDump, true))];
    }
    // Áp lại tab đã nhớ của pane phải (khoá ngữ nghĩa); không khớp -> tab đầu.
    NSInteger ri = [self tabIndexForKey:_rightPaneActiveTabKey inTitles:_respTabTitles];
    if (ri >= (NSInteger)_respBuffers.count) ri = 0;
    _activeRespTab = ri;
    _respText.string = _respBuffers.count ? _respBuffers[ri] : @"";
    [self highlightActiveTab:_respTabButtons active:ri];
}

#pragma mark Status line

- (void)updateStatus:(NSString *)text {
    _statusLabel.stringValue = text ?: @"";
    _statusLabel.textColor = [NSColor blackColor];
}

// Giờ HH:mm:ss.SSS (độ chính xác millisecond) từ epoch ms. <=0 -> placeholder.
- (NSString *)clockFromEpochMs:(int64_t)ms {
    if (ms <= 0) return @"--:--:--.---";
    static NSDateFormatter *fmt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ fmt = [NSDateFormatter new]; fmt.dateFormat = @"HH:mm:ss.SSS"; });
    return [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:ms / 1000.0]];
}

// status | size | time | start - end. endMs = lúc nhận response; start = end - elapsed.
- (void)updateStatusFromResponse:(const core::ApiResponse &)r error:(BOOL)isErr endMs:(int64_t)endMs {
    NSString *code = r.statusCode ? [NSString stringWithFormat:@"%d", r.statusCode] : @"OK";
    NSString *size = (r.sizeBytes >= 1024) ? [NSString stringWithFormat:@"%.1fkb", r.sizeBytes / 1024.0]
                                           : [NSString stringWithFormat:@"%lldb", (long long)r.sizeBytes];
    int64_t startMs = (endMs > 0) ? endMs - (int64_t)r.elapsedMs : 0;   // suy ra mốc bắt đầu
    NSString *range = [NSString stringWithFormat:@"%@ - %@",
                       [self clockFromEpochMs:startMs], [self clockFromEpochMs:endMs]];
    _statusLabel.stringValue = [NSString stringWithFormat:@"%@ | %@ | %ldms | %@", code, size, r.elapsedMs, range];
    _statusLabel.textColor = (r.statusCode >= 400) ? [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0]
                                                   : [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1.0];
}

@end
