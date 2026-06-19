#import "windows/MainWindowController+Private.h"

@implementation MainWindowController (Editor)

#pragma mark Load / populate / sync

// Huỷ + vô hiệu request đang bay trước khi chuyển (§8.2 bước 2): callback đến trễ
// sẽ bị drop vì handle khác _currentHandle.
- (void)cancelInFlightForSwitch {
    if (!_sending) return;
    if (_engine) _engine->cancel(_currentHandle);
    _currentHandle = 0;                 // invalidate handle
    [self finishSending];
}

- (void)loadRequestAtRel:(NSString *)rel {
    if (!_engine) return;
    BOOL switching = (_hasRequest && S(rel) != _currentRel);
    if (switching) {
        [self autosaveCurrent];         // 1. A dirty -> tự lưu
        [self cancelInFlightForSwitch]; // 2. huỷ request đang bay của A
        // 2b. (CRASH_FIX §2.1) commit + deactivate input context của A TRƯỚC khi xoá Scintilla.
        // Xoá nội dung khi editor/response còn first responder = tạo input context treo -> crash
        // trong updateWindows (đặc biệt khi đang gõ IME tiếng Việt rồi chuyển nhanh request).
        OS9SafeEndEditing(_window, _reqText);
        OS9SafeEndEditing(_window, _respText);
        // 3. giải phóng buffer text của A (editor + response) + undo (§8.3)
        [_reqText clearContents];
        [_respText clearContents];
        // §8.5: thả response của A — view KHÔNG giữ bản thứ hai (body lớn -> RAM về baseline).
        [_respBuffers removeAllObjects];
        _lastResp = core::ApiResponse{};
        _hasResp = NO;
    }
    try {
        _model = _engine->collection().loadRequest(rel.UTF8String);
        _currentRel = rel.UTF8String;
        _currentId = _model.id;          // theo dõi request đang mở bằng id ổn định
        _hasRequest = YES;
        _hasResp = NO;
        [self setRequestType:_model.type];
        [self populateEditorsFromModel];
        [self setHasRequest:YES];
        _engine->session().saveLastOpened(rel.UTF8String);
        [self updateTitle];
        [self relayout];
        [self updateStatus:@""];
        _respText.string = @"";
        [self showCachedResponseForId:_currentId];   // hiện response gần nhất (nếu có) — không gửi lại
        // reveal + unfold + highlight node đang mở (dùng id từ tên file, chỉ mở nhánh tổ tiên).
        [self revealAndSelectRequestById:N(_currentId) relPath:N(_currentRel)];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// Mở request -> tra cache (RAM trước, rồi disk) và dựng lại pane response nếu trúng (§0).
// §1.3: getResponse có thể đọc đĩa + parse JSON (response lớn) -> chạy NỀN, render trên main.
// Guard _currentId == reqId lúc hoàn tất để KHÔNG hiển thị response của request đã chuyển đi.
- (void)showCachedResponseForId:(const std::string &)reqId {
    if (reqId.empty() || !_engine) return;
    std::string idCopy = reqId;
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws;
        if (!s || !s->_engine) return;
        auto rec = s->_engine->getResponse(idCopy);    // I/O đĩa NGOÀI main thread
        if (!rec) return;
        __block core::ResponseRecord recCopy = std::move(*rec);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws;
            if (!s2 || s2->_currentId != idCopy) return;   // request đã chuyển -> bỏ kết quả cũ
            if (recCopy.isError) {
                [s2 displayErrorKind:recCopy.errorKind message:N(recCopy.errorMessage)];
            } else {
                s2->_lastResp = recCopy.response;
                s2->_hasResp = YES;
                [s2 rebuildResponseBuffers];
                [s2 updateStatusFromResponse:recCopy.response error:NO endMs:recCopy.receivedAt];
            }
        });
    });
}

- (void)populateEditorsFromModel {
    [_reqBuffers removeAllObjects];
    using namespace core;
    if (_model.type == RequestType::Http) {
        HttpRequest &h = _model.http;
        // Body mặc định JSON: request chưa có body (mode none) -> hiển thị như JSON.
        if (h.body.mode == "none") { h.body = Body{}; h.body.mode = "json"; }
        // Buffer body = ĐÚNG nội dung muốn gửi (không bọc {"mode","json"}); mode do app giữ.
        [_reqBuffers addObject:[self bodyBufferFromModel:h.body]];     // 0 = Body (ngoài cùng trái)
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(h.params))];   // 1
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(h.headers))];  // 2
        [_reqBuffers addObject:N(fieldcodec::authToJson(h.auth))];          // 3
        [_methodPopup selectTitle:N(h.method)];
        _urlField.stringValue = N(h.url); _urlPrevLen = _urlField.stringValue.length;
        _bodyMode = N(h.body.mode);
        [self updateBodyButtonLabel];
    } else {
        const GrpcRequest &g = _model.grpc;
        [_reqBuffers addObject:N(g.message.empty() ? "{}" : g.message)];
        [_reqBuffers addObject:N(fieldcodec::keyValuesToJson(g.metadata))];
        Auth dummy;
        [_reqBuffers addObject:N(fieldcodec::authToJson(dummy))];
        _urlField.stringValue = N(g.target); _urlPrevLen = _urlField.stringValue.length;
        // reflection -> index 0; protoFiles/descriptorSet -> ".proto" (index 1).
        _protoPopup.selectedIndex = (g.protoSource.mode == "reflection") ? 0 : 1;
        [_protoPopup setNeedsDisplay:YES];
        [self showSavedGrpcMethodLabel];   // hiện RPC đã lưu (KHÔNG fetch; fetch khi bấm dropdown)
    }
    // Áp LẠI tab đã nhớ của pane trái (nếu khoá tồn tại trong loại request hiện tại); else tab đầu.
    NSInteger li = [self tabIndexForKey:_leftPaneActiveTabKey inTitles:_reqTabTitles];
    if (li >= (NSInteger)_reqBuffers.count) li = 0;
    _activeReqTab = li;
    _reqText.string = _reqBuffers.count ? _reqBuffers[li] : @"";
    [self highlightActiveTab:_reqTabButtons active:li];
}

// Index của tab theo KHOÁ (title) trong bộ titles; không khớp/nil -> 0 (tab đầu của loại đó).
- (NSInteger)tabIndexForKey:(NSString *)key inTitles:(NSArray<NSString *> *)titles {
    if (key.length) {
        NSInteger idx = [titles indexOfObject:key];
        if (idx != NSNotFound) return idx;
    }
    return 0;
}

- (void)stashActiveReqBuffer {
    // [copy] BẮT BUỘC: NSTextView.string trả tham chiếu tới text storage SỐNG;
    // không copy -> buffer các tab cùng trỏ 1 chuỗi đang đổi -> giá trị lẫn vào nhau.
    if (_activeReqTab >= 0 && _activeReqTab < (NSInteger)_reqBuffers.count)
        _reqBuffers[_activeReqTab] = [(_reqText.string ?: @"") copy];
}

// Trả NO nếu JSON sai (báo toast + chọn tab). silent=YES -> không đổi tab/không toast (autosave).
- (BOOL)syncModelFromEditors:(BOOL)silent {
    [self stashActiveReqBuffer];
    using namespace core;
    std::string err;
    NSArray<NSString *> *names = _reqTabTitles;
    auto fail = [&](NSInteger tab, const std::string &e) {
        if (!silent) {
            [self selectReqTab:tab];
            NSString *tn = (tab >= 0 && tab < (NSInteger)names.count) ? names[tab] : @"?";
            [self toastWarn:[NSString stringWithFormat:@"Invalid JSON in tab %@: %s", tn, e.c_str()]];
        }
        return NO;
    };
    if (_model.type == RequestType::Http) {
        HttpRequest &h = _model.http;
        h.method = _methodPopup.selectedTitle.UTF8String ?: "GET";
        h.url = _urlField.stringValue.UTF8String;
        if (![self syncBodyFromBuffer:_reqBuffers[0] into:h.body err:err]) return fail(0, err);
        if (!fieldcodec::jsonToKeyValues(S(_reqBuffers[1]), h.params, err)) return fail(1, err);
        if (!fieldcodec::jsonToKeyValues(S(_reqBuffers[2]), h.headers, err)) return fail(2, err);
        if (!fieldcodec::jsonToAuth(S(_reqBuffers[3]), h.auth, err)) return fail(3, err);
    } else {
        GrpcRequest &g = _model.grpc;
        g.target = _urlField.stringValue.UTF8String;
        g.message = S(_reqBuffers[0]);
        if (!fieldcodec::jsonToKeyValues(S(_reqBuffers[1]), g.metadata, err)) return fail(1, err);
    }
    return YES;
}

// Nếu request đang mở nằm trong danh sách item bị xoá -> ĐÓNG editor để autosave KHÔNG tạo lại file.
// So khớp theo id ổn định (fallback relPath).
- (void)closeEditorIfDeleted:(NSArray<TreeItem *> *)deleted {
    for (TreeItem *t in deleted) {
        BOOL match = (_currentId.size() && t.requestId.length && S(t.requestId) == _currentId) ||
                     (S(t.relPath) == _currentRel && !_currentRel.empty());
        if (match) { _currentRel.clear(); _currentId.clear(); [self setHasRequest:NO]; return; }
    }
}

// Tự lưu mọi thay đổi (không hỏi). JSON sai -> bỏ qua + cảnh báo nhẹ.
- (void)autosaveCurrent {
    if (!_hasRequest || !_engine || _currentRel.empty()) return;
    if (![self resyncCurrentRelById]) return;     // request đã bị xoá/đổi path -> không ghi lại path cũ
    if (![self syncModelFromEditors:YES]) { [self toastWarn:@"Autosave failed: invalid JSON"]; return; }
    try { _currentRel = _engine->collection().saveRequest(_currentRel, _model);  // tên file có thể đổi (sync §4)
          _engine->session().saveLastOpened(_currentRel);
          [self reloadTree]; }
    catch (...) {}
}

#pragma mark Tabs

#pragma mark Body dropdown (json/file/form)

// Tên hiển thị nút Body theo mode hiện tại: "Body (JSON)" / "Body (FILE)" / "Body (FORM)".
- (NSString *)bodyButtonTitle {
    NSString *m = _bodyMode.length ? _bodyMode : @"json";
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:m]) return d[@"label"];
    return @"JSON";   // text/xml/none -> JSON (giữ hành vi cũ)
}
- (NSInteger)bodyTabIndex { return [_reqTabTitles indexOfObject:@"Body"]; } // 0 cho HTTP, NSNotFound cho gRPC
- (void)updateBodyButtonLabel {
    NSInteger bi = [self bodyTabIndex];
    if (bi == NSNotFound || bi >= (NSInteger)_reqTabButtons.count) return;
    _reqTabButtons[bi].title = [self bodyButtonTitle];
}
// Template body cho từng mode — KHÔNG bọc key "mode" (app tự giữ mode), nhưng CÓ sẵn
// các key gợi ý để người dùng biết điền gì.
//   json -> JSON thô.   form -> 1 entry key/value mẫu.   file -> object có key filePath.
static NSString *const kFormBodyTemplate =
    @"[\n  {\n    \"key\": \"\",\n    \"value\": \"\",\n    \"enabled\": true\n  }\n]";
static NSString *const kFileBodyTemplate = @"{\n  \"filePath\": \"\"\n}";

// NGUỒN DUY NHẤT cho dropdown Body: mode nội bộ <-> option/label/template.
// Thêm/bớt định dạng chỉ cần sửa bảng này (trước đây rải rác ở 3 hàm if-else).
static NSArray<NSDictionary *> *BodyModeTable(void) {
    static NSArray *t;
    if (!t) t = @[
        @{@"mode" : @"json",            @"opt" : @"JSON", @"label" : @"JSON", @"tpl" : @"{}"},
        @{@"mode" : @"binary",          @"opt" : @"File", @"label" : @"File", @"tpl" : kFileBodyTemplate},
        @{@"mode" : @"form-urlencoded", @"opt" : @"Form", @"label" : @"Form", @"tpl" : kFormBodyTemplate},
    ];
    return t;
}
- (NSString *)bodyTemplateForMode:(NSString *)mode {
    for (NSDictionary *d in BodyModeTable()) if ([d[@"mode"] isEqualToString:mode]) return d[@"tpl"];
    return @"{}";                                        // json (mặc định cho text/xml/none)
}

// Model.Body -> nội dung hiển thị trong editor (đúng cái gửi đi, không bọc).
- (NSString *)bodyBufferFromModel:(const core::Body &)b {
    if (b.mode == "form-urlencoded")
        return b.formUrlEncoded.empty() ? kFormBodyTemplate
                                        : N(core::fieldcodec::keyValuesToJson(b.formUrlEncoded));
    if (b.mode == "binary") {
        NSString *p = N(b.binaryFilePath);
        return [NSString stringWithFormat:@"{\n  \"filePath\": \"%@\"\n}", p ?: @""];
    }
    if (b.mode == "text") return N(b.text);
    if (b.mode == "xml") return N(b.xml);
    return N(b.json.empty() ? "{}" : b.json);            // json (mặc định)
}

// Editor buffer -> Model.Body, parse theo _bodyMode (mode do app định nghĩa, không nằm trong text).
- (BOOL)syncBodyFromBuffer:(NSString *)buf into:(core::Body &)out err:(std::string &)err {
    using namespace core;
    NSString *bm = _bodyMode.length ? _bodyMode : @"json";
    Body nb;
    if ([bm isEqualToString:@"form-urlencoded"]) {
        nb.mode = "form-urlencoded";
        if (!fieldcodec::jsonToKeyValues(S(buf), nb.formUrlEncoded, err)) return NO;
    } else if ([bm isEqualToString:@"binary"]) {
        nb.mode = "binary";
        // Nhận cả object {"filePath": "..."} lẫn chuỗi path thuần (tương thích cũ).
        NSString *t = [buf stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        id obj = t.length ? [NSJSONSerialization JSONObjectWithData:[t dataUsingEncoding:NSUTF8StringEncoding]
                                                            options:0 error:nil] : nil;
        if ([obj isKindOfClass:[NSDictionary class]]) {
            NSString *fp = ((NSDictionary *)obj)[@"filePath"] ?: ((NSDictionary *)obj)[@"path"];
            nb.binaryFilePath = fp.length ? S(fp) : "";
        } else {
            nb.binaryFilePath = S(t);   // coi cả text là path
        }
    } else if ([bm isEqualToString:@"text"]) {
        nb.mode = "text"; nb.text = S(buf);
    } else if ([bm isEqualToString:@"xml"]) {
        nb.mode = "xml"; nb.xml = S(buf);
    } else {
        nb.mode = "json"; nb.json = S(buf);   // JSON thô người dùng nhập, KHÔNG encode vào key "json"
    }
    out = nb;
    return YES;
}

- (void)bodyButtonClicked:(OS9BevelButton *)b {
    NSInteger bi = [self bodyTabIndex];
    if (bi == NSNotFound) return;
    NSMutableArray<NSString *> *opts = [NSMutableArray array];
    NSMutableArray<NSString *> *modes = [NSMutableArray array];
    for (NSDictionary *d in BodyModeTable()) { [opts addObject:d[@"opt"]]; [modes addObject:d[@"mode"]]; }
    NSInteger sel = [modes indexOfObject:(_bodyMode.length ? _bodyMode : @"json")];
    if (sel == NSNotFound) sel = 0;
    __weak MainWindowController *ws = self;
    OS9ShowDropdown(opts, sel, b, ^(NSInteger idx) {
        MainWindowController *s = ws; if (!s) return;
        [s pickBodyMode:modes[idx]];
    });
}

- (void)pickBodyMode:(NSString *)mode {
    NSInteger bi = [self bodyTabIndex];
    if (bi == NSNotFound) return;
    [self stashActiveReqBuffer];                 // lưu nội dung đang gõ vào tab hiện tại
    if (![_bodyMode isEqualToString:mode])       // đổi mode -> nạp template mới
        _reqBuffers[bi] = [self bodyTemplateForMode:mode];
    _bodyMode = mode;
    [self updateBodyButtonLabel];
    _activeReqTab = bi;                          // kích hoạt + hiện body
    if (bi < (NSInteger)_reqTabTitles.count) _leftPaneActiveTabKey = _reqTabTitles[bi];
    _reqText.string = _reqBuffers[bi];
    [self highlightActiveTab:_reqTabButtons active:bi];
}

#pragma mark Tabs

- (void)reqTabClicked:(OS9BevelButton *)b { [self selectReqTab:b.tag]; }
- (void)selectReqTab:(NSInteger)tab {
    if (tab < 0 || tab >= (NSInteger)_reqBuffers.count) return;
    [self stashActiveReqBuffer];
    _activeReqTab = tab;
    if (tab < (NSInteger)_reqTabTitles.count) _leftPaneActiveTabKey = _reqTabTitles[tab];  // nhớ pane trái
    _reqText.string = _reqBuffers[tab];
    [self highlightActiveTab:_reqTabButtons active:tab];
}
- (void)respTabClicked:(OS9BevelButton *)b {
    NSInteger tab = b.tag;
    if (tab < 0 || tab >= (NSInteger)_respBuffers.count) return;
    _activeRespTab = tab;
    if (tab < (NSInteger)_respTabTitles.count) _rightPaneActiveTabKey = _respTabTitles[tab];  // nhớ pane phải
    _respText.string = _respBuffers[tab];
    [self highlightActiveTab:_respTabButtons active:tab];
}
- (void)highlightActiveTab:(NSArray<OS9BevelButton *> *)buttons active:(NSInteger)active {
    // Tab đang chọn vẽ LÕM (selected) thay vì viền đậm (isDefault) — hợp phong cách OS9.
    for (OS9BevelButton *b in buttons) b.selected = (b.tag == active);
}
- (void)prettyToggle:(id)sender {
    _prettyMode = (_prettyMode + 1) % 4;   // Pretty -> Raw -> Encode -> Decode -> ...
    _prettyButton.title = [self prettyTitle];
    [self applyPrettyToFocusedPane];
}

// Áp chế độ hiện tại lên Ô ĐANG CÓ CON TRỎ: editor request (tab đang mở), setting,
// hoặc (mặc định) pane response. Nút bevel không nhận focus nên firstResponder giữ nguyên.
- (void)applyPrettyToFocusedPane {
    // request editor (Scintilla) đang giữ con trỏ?
    if ([_reqText hasFocus]) {
        _reqText.string = [self applyView:S(_reqText.string)];
        [self stashActiveReqBuffer];
        return;
    }
    // setting editor (Scintilla) đang giữ con trỏ?
    if ([_settingEditor hasFocus]) {
        _settingEditor.string = [self applyView:S(_settingEditor.string)];
        return;
    }
    if (_hasResp) [self rebuildResponseBuffers]; // mặc định: pane response
}

// Copy request hiện tại dạng cURL (HTTP) / grpcurl (gRPC) vào clipboard.
- (void)copyAsCurl:(id)sender {
    if (!_hasRequest || !_engine) return;
    if (![self syncModelFromEditors:NO]) return;
    core::ResolvedRequest rr = _engine->resolveRequest(_model);
    std::string curl = core::toCurl(rr.model);
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:N(curl) forType:NSPasteboardTypeString];
    [self toastOk:@"Copied as cURL"];
}

// Zoom toggle thủ công (performZoom đôi khi không thu nhỏ lại được).
- (void)zoomToggle:(id)sender {
    NSScreen *sc = _window.screen ?: [NSScreen mainScreen];
    NSRect vis = sc.visibleFrame;
    if (NSEqualRects(_window.frame, vis)) {
        if (!NSIsEmptyRect(_preZoomFrame)) [_window setFrame:_preZoomFrame display:YES animate:YES];
    } else {
        _preZoomFrame = _window.frame;
        [_window setFrame:vis display:YES animate:YES];
    }
}

// Minimize: thu cửa sổ vào Dock (genie). Window borderless + Miniaturizable -> miniaturize: chạy.
- (void)collapseToggle:(id)sender { [_window miniaturize:nil]; }

// Áp font cấu hình (từ Settings) cho mọi ô chữ + vẽ lại.
- (void)applyConfiguredFontAndRefresh {
    core::AppConfigStore a; a.setDefaults([self appDefaultsFromEnv]); core::AppConfig c = a.load();
    [OS9Theme setConfiguredFontName:N(c.fontName) size:c.fontSize];
    NSFont *mono = [OS9Theme monoFont];
    [_reqText setFontName:N(c.fontName) size:c.fontSize];
    [_respText setFontName:N(c.fontName) size:c.fontSize];
    [_settingEditor setFontName:N(c.fontName) size:c.fontSize];
    _urlField.font = mono;
    _tree.font = [OS9Theme uiFont];
    [_tree reloadData];
    [self restoreExpansion:_roots];
    [_window.contentView setNeedsDisplay:YES];
}

#pragma mark Editing

// Dán cURL/grpcurl vào ô URL -> tự nhận biết -> preview -> tạo request mới (CURL_IMPORT.md).
// Nhận biết "dán/drop" bằng độ dài tăng đột biến (>=8 ký tự một lần) — gõ tay tăng 1/ký tự.
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object != _urlField) return;
    NSString *text = _urlField.stringValue ?: @"";
    NSUInteger len = text.length;
    NSUInteger prev = _urlPrevLen;
    _urlPrevLen = len;
    if (!_engine || len < prev + 8) return;            // không phải dán -> bỏ qua
    BOOL isCurl = _engine->looksLikeCurl(text.UTF8String);
    BOOL isGrpc = !isCurl && _engine->looksLikeGrpcurl(text.UTF8String);
    if (!isCurl && !isGrpc) return;
    // cURL: tự import + tạo request luôn (chỉ toast, KHÔNG popup). grpcurl: vẫn xác nhận popup.
    // Defer: tránh xử lý NGAY trong callback đổi text (field editor đang bận).
    dispatch_async(dispatch_get_main_queue(), ^{
        if (isCurl) [self importNow:text grpc:NO];
        else        [self offerImport:text grpc:YES];
    });
}

// Import + tạo request ngay, không hỏi; báo kết quả qua toast.
- (void)importNow:(NSString *)text grpc:(BOOL)isGrpc {
    core::ImportResult r = isGrpc ? _engine->importFromGrpc(text.UTF8String)
                                  : _engine->importFromCurl(text.UTF8String);
    if (!r.ok) {
        [self toastWarn:[NSString stringWithFormat:@"Import %@ failed: %s",
                         isGrpc ? @"grpcurl" : @"cURL", r.error.c_str()]];
        [self restoreUrlField];
        return;
    }
    [self applyImport:r.model];
}

// Hiện preview xác nhận; nếu OK -> tạo request mới trong tree + mở editor.
- (void)offerImport:(NSString *)text grpc:(BOOL)isGrpc {
    core::ImportResult r = isGrpc ? _engine->importFromGrpc(text.UTF8String)
                                  : _engine->importFromCurl(text.UTF8String);
    if (!r.ok) {
        [self toastWarn:[NSString stringWithFormat:@"Import %@ failed: %s",
                         isGrpc ? @"grpcurl" : @"cURL", r.error.c_str()]];
        return;
    }
    NSString *primary = _hasRequest ? @"Replace current" : @"Create request";
    NSString *body = [NSString stringWithFormat:@"%@\n\n%@",
                      isGrpc ? @"grpcurl command detected" : @"cURL command detected",
                      [self importSummary:r.model unknown:r.unknown grpc:isGrpc]];
    NSInteger choice = [OS9Dialog confirmWithTitle:@"Import"
                                           message:body
                                           buttons:@[ @"Cancel", primary ]
                                     defaultButton:1 cancelButton:0
                                              icon:OS9AlertNote
                                            parent:_window];
    if (choice == 1) [self applyImport:r.model];
    else [self restoreUrlField];   // bỏ: trả ô URL về giá trị request đang mở
}

- (NSString *)importSummary:(const core::RequestModel &)m unknown:(const std::vector<std::string> &)unknown grpc:(BOOL)isGrpc {
    NSMutableString *s = [NSMutableString string];
    if (isGrpc) {
        const core::GrpcRequest &g = m.grpc;
        [s appendFormat:@"target: %s\n", g.target.c_str()];
        [s appendFormat:@"%s / %s\n", g.service.c_str(), g.method.c_str()];
        [s appendFormat:@"TLS: %@ · metadata: %lu · proto: %s",
            g.tls.enabled ? @"secure" : @"plaintext",
            (unsigned long)g.metadata.size(), g.protoSource.mode.c_str()];
    } else {
        const core::HttpRequest &h = m.http;
        [s appendFormat:@"%s  %s\n", h.method.c_str(), h.url.c_str()];
        [s appendFormat:@"headers: %lu · body: %s · auth: %s",
            (unsigned long)h.headers.size(), h.body.mode.c_str(), h.auth.type.c_str()];
    }
    if (!unknown.empty()) {
        [s appendString:@"\nskipped:"];
        for (const auto &u : unknown) [s appendFormat:@" %s", u.c_str()];
    }
    return s;
}

// Tên gợi ý: HTTP "METHOD lastPathSegment"; gRPC = method.
- (NSString *)deriveImportName:(const core::RequestModel &)m {
    if (m.type == core::RequestType::Grpc)
        return m.grpc.method.empty() ? @"Imported gRPC" : N(m.grpc.method);
    NSString *url = N(m.http.url);
    NSString *path = url;
    NSRange q = [path rangeOfString:@"?"]; if (q.location != NSNotFound) path = [path substringToIndex:q.location];
    NSString *last = path.lastPathComponent;
    if (!last.length || [last containsString:@":"]) last = @"request"; // chỉ có host
    return [NSString stringWithFormat:@"%s %@", m.http.method.c_str(), last];
}

// REPLACE request đang mở bằng model import (giữ id/name/file, thay type + payload).
// Không có request nào đang mở -> tạo mới (fallback).
- (void)applyImport:(const core::RequestModel &)m {
    if (!_hasRequest || _currentRel.empty() || ![self resyncCurrentRelById]) {
        NSString *name = [self deriveImportName:m];   // fallback: chưa mở request -> tạo mới
        try {
            std::string rel = _engine->collection().createRequestFromModel([self selectedFolderRel], m, name.UTF8String);
            [self reloadTree];
            [self loadRequestAtRel:N(rel)];
            [self toastOk:[NSString stringWithFormat:@"Imported & created: %@", name]];
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
        return;
    }
    // Replace tại chỗ: giữ id + name của request hiện tại; URL/target hiện giá trị đã parse.
    core::RequestModel n = m;
    n.id = _model.id;
    n.name = _model.name;
    _model = n;
    _hasResp = NO;
    [self setRequestType:_model.type];   // rebuild tab theo type mới (http <-> grpc)
    [self populateEditorsFromModel];
    [self setHasRequest:YES];
    _respText.string = @"";              // xoá response cũ
    [self updateTitle];
    [self relayout];
    try {
        _currentRel = _engine->collection().saveRequest(_currentRel, _model);   // lưu + sync tên file (§4)
        _engine->session().saveLastOpened(_currentRel);
        [self reloadTree];
        [self toastOk:[NSString stringWithFormat:@"Replaced current request (%@)",
                       _model.type == core::RequestType::Grpc ? @"gRPC" : @"HTTP"]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// Trả ô URL về URL/target của request đang mở (tránh lưu nhầm text lệnh).
- (void)restoreUrlField {
    if (!_hasRequest) { _urlField.stringValue = @""; _urlPrevLen = 0; return; }
    NSString *u = (_model.type == core::RequestType::Grpc) ? N(_model.grpc.target) : N(_model.http.url);
    _urlField.stringValue = u;
    _urlPrevLen = u.length;
}

- (void)methodChanged:(id)sender { }
- (void)urlCommitted:(id)sender {
    // gRPC: ô URL = target -> Enter nạp lại danh sách RPC. HTTP: tách query.
    if (_model.type == core::RequestType::Grpc) [self reloadGrpcMethods];
    else [self parseUrlQueryIntoQueryTab];
}

// Decode 1 thành phần query: '+' -> space, %XX -> byte (khớp Core urlutil::urlDecode).
- (NSString *)urlDecodeComponent:(NSString *)s {
    NSString *plus = [s stringByReplacingOccurrencesOfString:@"+" withString:@" "];
    return [plus stringByRemovingPercentEncoding] ?: plus;
}

// Nếu ô URL có '?...': tách query (decode) -> nối vào tab Query, ô URL còn raw.
// Dùng khi user tự gõ query vào URL rồi Enter/Send (giống hành vi import cURL).
- (void)parseUrlQueryIntoQueryTab {
    NSInteger qi = [_reqTabTitles indexOfObject:@"Query"];
    if (qi == NSNotFound || qi >= (NSInteger)_reqBuffers.count) return;   // gRPC: không có Query
    NSString *u = _urlField.stringValue ?: @"";
    NSRange qr = [u rangeOfString:@"?"];
    if (qr.location == NSNotFound) return;                                // không có query
    NSString *raw = [u substringToIndex:qr.location];
    NSString *query = [u substringFromIndex:qr.location + 1];
    NSRange hr = [query rangeOfString:@"#"];
    if (hr.location != NSNotFound) query = [query substringToIndex:hr.location];

    [self stashActiveReqBuffer];   // buffer tab Query hiện tại là mới nhất trước khi nối thêm

    // Lấy các entry sẵn có trong tab Query (JSON array) rồi nối entry parse được.
    NSMutableArray *items = [NSMutableArray array];
    NSData *cur = [(_reqBuffers[qi] ?: @"[]") dataUsingEncoding:NSUTF8StringEncoding];
    id arr = cur ? [NSJSONSerialization JSONObjectWithData:cur options:0 error:nil] : nil;
    if ([arr isKindOfClass:[NSArray class]]) [items addObjectsFromArray:arr];
    for (NSString *seg in [query componentsSeparatedByString:@"&"]) {
        if (!seg.length) continue;
        NSRange eq = [seg rangeOfString:@"="];
        NSString *k = (eq.location == NSNotFound) ? seg : [seg substringToIndex:eq.location];
        NSString *v = (eq.location == NSNotFound) ? @"" : [seg substringFromIndex:eq.location + 1];
        [items addObject:@{@"key" : [self urlDecodeComponent:k],
                           @"value" : [self urlDecodeComponent:v], @"enabled" : @YES}];
    }
    NSData *out = [NSJSONSerialization dataWithJSONObject:items options:NSJSONWritingPrettyPrinted error:nil];
    if (out) _reqBuffers[qi] = [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding];

    _urlField.stringValue = raw; _urlPrevLen = raw.length;       // ô URL còn raw
    if (_activeReqTab == qi) _reqText.string = _reqBuffers[qi];   // đang xem tab Query -> refresh
}

- (void)updateTitle {
    // Title CHỈ là tên request (rỗng nếu chưa chọn). Không còn dấu dirty.
    _titleBar.title = _hasRequest ? N(_model.name) : @"";
    [_titleBar setNeedsDisplay:YES];
}

#pragma mark Save (thủ công vẫn giữ ⌘S)

- (void)saveRequest:(id)sender {
    if (!_hasRequest || !_engine) return;
    if (![self resyncCurrentRelById]) { [self toastWarn:@"Request no longer exists"]; return; }
    if (![self syncModelFromEditors:NO]) return;
    try {
        _currentRel = _engine->collection().saveRequest(_currentRel, _model);  // tên file đồng bộ method/name (§4)
        _engine->session().saveLastOpened(_currentRel);
        [self reloadTree];
        [self toastOk:@"Saved"];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

@end
