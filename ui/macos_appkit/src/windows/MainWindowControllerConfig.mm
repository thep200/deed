#import "windows/MainWindowControllerPrivate.h"

// Base column: internal KEY "Global" (keeps {{var}} resolve semantics) but DISPLAYED as "Local"
// (SPEC §T4). Mapped both ways at the UI layer.
static NSString *const kBaseEnvKey = @"Global";
static NSString *EnvDisplay(NSString *key) {
    return [key isEqualToString:kBaseEnvKey] ? StrEnvLocal : key;
}
static NSString *EnvKeyFromDisplay(NSString *disp) {
    return [disp isEqualToString:StrEnvLocal] ? kBaseEnvKey : disp;
}

@implementation MainWindowController (Config)

#pragma mark ENV

- (void)envClicked:(id)sender {
    if (!_engine) { [self toastWarn:StrToastOpenFolderFirst]; return; }
    NSMutableArray<NSString *> *items = [@[ StrEnvLocal ] mutableCopy];   // base displayed as "Local"
    for (const auto &name : _engine->environments().list())
        if (name != kBaseEnvKey.UTF8String) [items addObject:N(name)];
    [items addObject:StrEnvManage];
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
    [self toast:[NSString stringWithFormat:StrFmtToastEnv, name]];
}
- (void)refreshEnvButton {
    _envButton.title = _engine ? EnvDisplay(N(_engine->session().getActiveEnv())) : StrEnvLocal;
}

#pragma mark Config screen (ENV + Setting)

// Setting button -> Settings screen; ENV "Manage…" -> Environments screen (2 separate screens).
- (void)settingClicked:(id)sender { [self enterConfig:1]; }

- (void)enterConfig:(NSInteger)kind {
    if (!_engine) { [self toastWarn:StrToastOpenFolderFirst]; return; }
    // §2.1: release the main pane's input context (URL/editor) before hiding it.
    OS9SafeEndEditing(_window, nil);
    [self autosaveCurrent];
    _configKind = kind;
    if (kind == 0) {
        NSView *ev = _envVC.view;
        if (ev.superview != _configPane) [_configPane addSubview:ev];
        ev.hidden = NO;
        _settingInset.hidden = YES;
        [_envVC reload];
    } else {
        if (_envVC.view) _envVC.view.hidden = YES;
        _settingInset.hidden = NO;
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
    [self updateTitle];   // title bar -> "Settings"/"Environments"
    [self relayout];
}

- (void)exitConfig:(id)sender {
    // §2.1: commit + release the editing field's input context (settings editor / env cell) BEFORE
    // hiding the config pane — otherwise the hidden view still holds the input context -> crash in updateWindows.
    OS9SafeEndEditing(_window, nil);
    // Auto-save on back, for whichever screen is open.
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
                _engine->reloadCacheConfig();   // apply new cap/threshold -> evict immediately if smaller (§1.2)
                [self applyConfiguredFontAndRefresh];
            } else {
                [self toastWarn:StrToastInvalidSettings];
            }
        }
    }
    _configMode = NO;
    _configPane.hidden = YES;
    _mainPane.hidden = NO;
    [self refreshEnvButton];
    [self updateTitle];   // title bar -> current request name
    [self relayout];
    [self toastOk:StrToastSaved];
}

#pragma mark Proto source (gRPC)

// Proto source dropdown: index 0 = Reflection, 1 = .proto (opens file panel).
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
            // Cancel file selection -> revert to previous state (reflection).
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

// Short, tagged RPC label for the picker, e.g. "[Unary] Calc/Add". The service is shortened to its last
// dotted segment (calc.Calc -> Calc); the leading tag shows the streaming direction.
static NSString *GrpcRpcLabel(const std::string &service, const std::string &method,
                              const std::string &methodType) {
    std::string svc = service;
    auto dot = svc.find_last_of('.');
    if (dot != std::string::npos) svc = svc.substr(dot + 1);
    NSString *tag = StrGrpcTagUnary;
    if (methodType == "server_streaming") tag = StrGrpcTagServerStream;
    else if (methodType == "client_streaming") tag = StrGrpcTagClientStream;
    else if (methodType == "bidi_streaming") tag = StrGrpcTagBidiStream;
    return [NSString stringWithFormat:@"%@ %s/%s", tag, svc.c_str(), method.c_str()];
}

// Show the RPC saved in the model on the button (NO network call). Real fetch happens on dropdown click.
- (void)showSavedGrpcMethodLabel {
    _grpcMethods.clear();
    const core::GrpcRequest &g = _model.grpc;
    if (g.service.empty() || g.method.empty()) {
        _servicePopup.itemTitles = @[ StrNoRpc ];
        _servicePopup.toolTip = nil;
    } else {
        NSString *full = [NSString stringWithFormat:@"%s/%s", g.service.c_str(), g.method.c_str()];
        _servicePopup.itemTitles = @[ GrpcRpcLabel(g.service, g.method, g.methodType) ];
        _servicePopup.toolTip = full;   // hover shows the fully-qualified pkg.Service/Method
    }
    _servicePopup.selectedIndex = 0;
    [_servicePopup setNeedsDisplay:YES];
}

// Background load (does NOT open menu): used when changing proto source / committing URL.
- (void)reloadGrpcMethods { [self fetchGrpcMethodsThenOpen:NO]; }

// Load the service/RPC list from the current proto source (reflection: query host; .proto: parse).
// openWhenDone = YES: open the menu right after loading (used when clicking the dropdown to pick an RPC).
// Runs in background because reflection does network IO; only applies the result of the latest call.
- (void)fetchGrpcMethodsThenOpen:(BOOL)openWhenDone {
    if (_model.type != core::RequestType::Grpc || !_engine) return;
    _model.grpc.target = _urlField.stringValue.UTF8String; // target = URL field (host:port)
    // Reflection needs a host; .proto parses a file so a host is not required.
    BOOL needsHost = (_model.grpc.protoSource.mode == "reflection");
    if (needsHost && _model.grpc.target.empty()) {
        _grpcMethods.clear();
        _servicePopup.itemTitles = @[ StrNoRpc ];
        _servicePopup.selectedIndex = 0;
        _servicePopup.toolTip = nil;
        [_servicePopup setNeedsDisplay:YES];
        if (openWhenDone) [self toastWarn:StrToastEnterGrpcHost];
        return;
    }
    _servicePopup.itemTitles = @[ StrLoading ];
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
            if (!s || seq != s->_grpcMethodsReqSeq) return; // a newer request already exists
            [s applyGrpcMethods:methods error:errStr openMenu:openWhenDone];
        });
    });
}

- (void)applyGrpcMethods:(const std::vector<core::GrpcMethodInfo> &)methods error:(NSString *)err
                openMenu:(BOOL)openMenu {
    _grpcMethods = methods;
    if (methods.empty()) {
        _servicePopup.itemTitles = @[ StrNoRpc ];
        _servicePopup.selectedIndex = 0;
        _servicePopup.toolTip = nil;
        [_servicePopup setNeedsDisplay:YES];
        if (err.length) [self toastWarn:[NSString stringWithFormat:StrFmtToastListRpcs, err]];
        return;
    }
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    NSInteger sel = 0;
    for (size_t i = 0; i < methods.size(); ++i) {
        const core::GrpcMethodInfo &m = methods[i];
        [titles addObject:GrpcRpcLabel(m.service, m.method, m.methodType)];
        if (m.service == _model.grpc.service && m.method == _model.grpc.method) sel = (NSInteger)i;
    }
    _servicePopup.itemTitles = titles;
    _servicePopup.selectedIndex = sel;
    [_servicePopup setNeedsDisplay:YES];
    [self applySelectedGrpcMethod:sel]; // sync model with displayed selection
    if (openMenu) [_servicePopup openMenu];
}

// User picks an RPC -> write service/method/methodType into the model (autosave persists it).
- (void)serviceMethodChanged:(id)sender {
    [self applySelectedGrpcMethod:_servicePopup.selectedIndex];
}

- (void)applySelectedGrpcMethod:(NSInteger)idx {
    if (idx < 0 || idx >= (NSInteger)_grpcMethods.size()) return;
    const core::GrpcMethodInfo &m = _grpcMethods[(size_t)idx];
    _model.grpc.service = m.service;
    _model.grpc.method = m.method;
    _model.grpc.methodType = m.methodType;
    // Hovering the button shows the full RPC name (the button may have truncated it with "…").
    _servicePopup.toolTip = [NSString stringWithFormat:@"%s/%s", m.service.c_str(), m.method.c_str()];
}

- (void)manageEnv:(id)sender { [self enterConfig:0]; }
@end
