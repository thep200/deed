#import "windows/MainWindowController+Private.h"

// Cột nền: KEY nội bộ "Global" (giữ ngữ nghĩa resolve {{var}}) nhưng HIỂN THỊ "Local"
// (SPEC §T4). Map qua lại ở lớp UI.
static NSString *const kBaseEnvKey = @"Global";
static NSString *const kBaseEnvLabel = @"Local";
static NSString *EnvDisplay(NSString *key) {
    return [key isEqualToString:kBaseEnvKey] ? kBaseEnvLabel : key;
}
static NSString *EnvKeyFromDisplay(NSString *disp) {
    return [disp isEqualToString:kBaseEnvLabel] ? kBaseEnvKey : disp;
}

@implementation MainWindowController (Config)

#pragma mark ENV

- (void)envClicked:(id)sender {
    if (!_engine) { [self toastWarn:@"Open a collection folder first"]; return; }
    NSMutableArray<NSString *> *items = [@[ kBaseEnvLabel ] mutableCopy];   // base hiển thị "Local"
    for (const auto &name : _engine->environments().list())
        if (name != kBaseEnvKey.UTF8String) [items addObject:N(name)];
    [items addObject:@"Manage…"];
    NSString *activeDisp = EnvDisplay(N(_engine->session().getActiveEnv()));
    NSInteger sel = [items indexOfObject:activeDisp]; if (sel == NSNotFound) sel = 0;
    __weak MainWindowController *ws = self;
    OS9ShowDropdown(items, sel, _envButton, ^(NSInteger idx) {
        MainWindowController *s = ws; if (!s) return;
        if (idx == (NSInteger)items.count - 1) { [s manageEnv:nil]; return; }
        [s pickEnvNamed:items[idx]];
    });
}
- (void)pickEnvNamed:(NSString *)name {
    if (!_engine) return;
    _engine->session().setActiveEnv(EnvKeyFromDisplay(name).UTF8String);   // "Local" -> key "Global"
    [self refreshEnvButton];
    [self toast:[NSString stringWithFormat:@"ENV: %@", name]];
}
- (void)refreshEnvButton {
    _envButton.title = _engine ? EnvDisplay(N(_engine->session().getActiveEnv())) : kBaseEnvLabel;
}

#pragma mark Config screen (ENV + Setting)

// Setting button -> màn Settings; ENV "Manage…" -> màn Environments (2 màn riêng).
- (void)settingClicked:(id)sender { [self enterConfig:1]; }

- (void)enterConfig:(NSInteger)kind {
    if (!_engine) { [self toastWarn:@"Open a collection folder first"]; return; }
    // §2.1: nhả input context của pane chính (URL/editor) trước khi ẩn nó đi.
    OS9SafeEndEditing(_window, nil);
    [self autosaveCurrent];
    _configKind = kind;
    if (kind == 0) {
        _configTitle.stringValue = @"Environments";
        NSView *ev = _envVC.view;
        if (ev.superview != _configPane) [_configPane addSubview:ev];
        ev.hidden = NO;
        _settingEditor.hidden = YES;
        [_envVC reload];
    } else {
        _configTitle.stringValue = @"Settings";
        if (_envVC.view) _envVC.view.hidden = YES;
        _settingEditor.hidden = NO;
        core::AppConfig c = _engine->appConfig().load();
        _settingEditor.string = [NSString stringWithFormat:
            @"{\n  \"default_timeout_ms\": %d,\n  \"verify_tls\": %@,\n  \"font_name\": \"%s\",\n  \"font_size\": %d,\n"
             "  \"ram_cache_size\": %d,\n  \"disk_cache_size\": %d\n}",
            c.defaultTimeoutMs, c.verifyTls ? @"true" : @"false", c.fontName.c_str(), c.fontSize,
            c.ramCacheSizeMb, c.diskCacheSizeMb];
    }
    _configMode = YES;
    _mainPane.hidden = YES;
    _configPane.hidden = NO;
    [self relayout];
}

- (void)exitConfig:(id)sender {
    // §2.1: commit + nhả input context của ô đang sửa (settings editor / env cell) TRƯỚC khi
    // ẩn config pane — nếu không, view bị ẩn vẫn treo input context -> crash ở updateWindows.
    OS9SafeEndEditing(_window, nil);
    // Auto-save khi back, theo đúng màn đang mở.
    if (_engine) {
        if (_configKind == 0) {
            [_envVC save];
        } else {
            NSData *d = [_settingEditor.string dataUsingEncoding:NSUTF8StringEncoding];
            NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
            if (dict) {
                core::AppConfig c = _engine->appConfig().load();
                if (dict[@"default_timeout_ms"]) c.defaultTimeoutMs = [dict[@"default_timeout_ms"] intValue];
                if (dict[@"verify_tls"]) c.verifyTls = [dict[@"verify_tls"] boolValue];
                if (dict[@"font_name"]) c.fontName = [dict[@"font_name"] UTF8String];
                if (dict[@"font_size"]) c.fontSize = [dict[@"font_size"] intValue];
                if (dict[@"ram_cache_size"]) c.ramCacheSizeMb = [dict[@"ram_cache_size"] intValue];
                if (dict[@"disk_cache_size"]) c.diskCacheSizeMb = [dict[@"disk_cache_size"] intValue];
                _engine->appConfig().save(c);
                _engine->reloadCacheConfig();   // áp cap/threshold mới -> evict ngay nếu nhỏ đi (§1.2)
                [self applyConfiguredFontAndRefresh];
            } else {
                [self toastWarn:@"Invalid settings JSON — skipped"];
            }
        }
    }
    _configMode = NO;
    _configPane.hidden = YES;
    _mainPane.hidden = NO;
    [self refreshEnvButton];
    [self relayout];
    [self toastOk:@"Saved"];
}

#pragma mark Proto source (gRPC)

// Dropdown nguồn proto: index 0 = Reflection, 1 = .proto (mở file panel).
- (void)protoModeChanged:(id)sender {
    if (_model.type != core::RequestType::Grpc) return;
    if (_protoPopup.selectedIndex == 1) {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.allowedFileTypes = @[ @"proto" ];
        if ([p runModal] == NSModalResponseOK) {
            core::ProtoSource ps;
            ps.mode = "protoFiles";
            ps.files.push_back(p.URL.lastPathComponent.UTF8String);
            ps.importPaths.push_back(p.URL.URLByDeletingLastPathComponent.path.UTF8String);
            _model.grpc.protoSource = ps;
        } else {
            // Huỷ chọn file -> trở về trạng thái trước (reflection).
            _protoPopup.selectedIndex = 0;
            [_protoPopup setNeedsDisplay:YES];
            _model.grpc.protoSource = core::ProtoSource{};
            _model.grpc.protoSource.mode = "reflection";
        }
    } else {
        _model.grpc.protoSource = core::ProtoSource{};
        _model.grpc.protoSource.mode = "reflection";
    }
    [self reloadGrpcMethods];
}

#pragma mark RPC picker (gRPC)

// Hiện RPC đã lưu trong model lên nút (KHÔNG gọi mạng). Fetch thật khi bấm vào dropdown.
- (void)showSavedGrpcMethodLabel {
    _grpcMethods.clear();
    const core::GrpcRequest &g = _model.grpc;
    if (g.service.empty() || g.method.empty()) {
        _servicePopup.itemTitles = @[ @"No RPC" ];
        _servicePopup.toolTip = nil;
    } else {
        NSString *full = [NSString stringWithFormat:@"%s/%s", g.service.c_str(), g.method.c_str()];
        _servicePopup.itemTitles = @[ full ];
        _servicePopup.toolTip = full;
    }
    _servicePopup.selectedIndex = 0;
    [_servicePopup setNeedsDisplay:YES];
}

// Nạp nền (KHÔNG bung menu): dùng khi đổi nguồn proto / commit URL.
- (void)reloadGrpcMethods { [self fetchGrpcMethodsThenOpen:NO]; }

// Nạp danh sách service/RPC theo nguồn proto hiện tại (reflection: query host; .proto: parse).
// openWhenDone = YES: bung menu ngay sau khi nạp xong (dùng khi bấm vào dropdown chọn RPC).
// Chạy nền vì reflection có IO mạng; chỉ áp kết quả của lần gọi mới nhất.
- (void)fetchGrpcMethodsThenOpen:(BOOL)openWhenDone {
    if (_model.type != core::RequestType::Grpc || !_engine) return;
    _model.grpc.target = _urlField.stringValue.UTF8String; // target = ô URL (host:port)
    // Reflection cần host; .proto thì parse file nên không bắt buộc host.
    BOOL needsHost = (_model.grpc.protoSource.mode == "reflection");
    if (needsHost && _model.grpc.target.empty()) {
        _grpcMethods.clear();
        _servicePopup.itemTitles = @[ @"No RPC" ];
        _servicePopup.selectedIndex = 0;
        _servicePopup.toolTip = nil;
        [_servicePopup setNeedsDisplay:YES];
        if (openWhenDone) [self toastWarn:@"Enter gRPC host first (e.g. localhost:50051)"];
        return;
    }
    _servicePopup.itemTitles = @[ @"Loading..." ];
    _servicePopup.selectedIndex = 0;
    [_servicePopup setNeedsDisplay:YES];

    uint64_t seq = ++_grpcMethodsReqSeq;
    core::GrpcRequest g = _model.grpc;
    core::Engine *engine = _engine.get();
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::string err;
        std::vector<core::GrpcMethodInfo> methods = engine->listGrpcMethods(g, err);
        NSString *errStr = err.empty() ? nil : N(err);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s = ws;
            if (!s || seq != s->_grpcMethodsReqSeq) return; // đã có yêu cầu mới hơn
            [s applyGrpcMethods:methods error:errStr openMenu:openWhenDone];
        });
    });
}

- (void)applyGrpcMethods:(const std::vector<core::GrpcMethodInfo> &)methods error:(NSString *)err
                openMenu:(BOOL)openMenu {
    _grpcMethods = methods;
    if (methods.empty()) {
        _servicePopup.itemTitles = @[ @"No RPC" ];
        _servicePopup.selectedIndex = 0;
        _servicePopup.toolTip = nil;
        [_servicePopup setNeedsDisplay:YES];
        if (err.length) [self toastWarn:[NSString stringWithFormat:@"List RPCs: %@", err]];
        return;
    }
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    NSInteger sel = 0;
    for (size_t i = 0; i < methods.size(); ++i) {
        const core::GrpcMethodInfo &m = methods[i];
        [titles addObject:[NSString stringWithFormat:@"%s/%s", m.service.c_str(), m.method.c_str()]];
        if (m.service == _model.grpc.service && m.method == _model.grpc.method) sel = (NSInteger)i;
    }
    _servicePopup.itemTitles = titles;
    _servicePopup.selectedIndex = sel;
    [_servicePopup setNeedsDisplay:YES];
    [self applySelectedGrpcMethod:sel]; // đồng bộ model với lựa chọn hiển thị
    if (openMenu) [_servicePopup openMenu];
}

// Người dùng chọn RPC -> ghi service/method/methodType vào model (autosave tự lưu).
- (void)serviceMethodChanged:(id)sender {
    [self applySelectedGrpcMethod:_servicePopup.selectedIndex];
}

- (void)applySelectedGrpcMethod:(NSInteger)idx {
    if (idx < 0 || idx >= (NSInteger)_grpcMethods.size()) return;
    const core::GrpcMethodInfo &m = _grpcMethods[(size_t)idx];
    _model.grpc.service = m.service;
    _model.grpc.method = m.method;
    _model.grpc.methodType = m.methodType;
    // Hover nút hiện tên RPC đầy đủ (nút có thể đã cắt "…").
    _servicePopup.toolTip = [NSString stringWithFormat:@"%s/%s", m.service.c_str(), m.method.c_str()];
}

- (void)manageEnv:(id)sender { [self enterConfig:0]; }
@end
