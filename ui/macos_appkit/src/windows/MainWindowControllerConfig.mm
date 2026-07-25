#import "windows/MainWindowControllerPrivate.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>   // UTType (NSOpenPanel.allowedContentTypes)

#include <type_traits>
#include <variant>

// No special base env: the dropdown lists every environment by its own name; the title shows the active
// env (or "ENV" when none is selected).
static NSString *const kEnvNone = @"ENV";

@implementation MainWindowController (Config)

#pragma mark ENV

- (void)envClicked:(id)sender {
    if (!_apiClient) { [self toastWarn:StrToastOpenFolderFirst]; return; }
    NSMutableArray<NSString *> *items = [NSMutableArray array];
    for (const auto &name : _apiClient->environments().list()) [items addObject:N(name)];
    [items addObject:StrEnvManage];
    NSString *active = N(_apiClient->session().getActiveEnv());
    NSInteger sel = [items indexOfObject:active]; if (sel == NSNotFound) sel = 0;
    __weak MainWindowController *ws = self;
    OS9ShowDropdown(items, sel, _envButton, ^(NSInteger idx) {
        MainWindowController *s = ws; if (!s) return;
        if (idx == (NSInteger)items.count - 1) { [s manageEnv:nil]; return; }
        [s pickEnvNamed:items[idx]];
    });
}
- (void)pickEnvNamed:(NSString *)name {
    if (!_apiClient) return;
    _apiClient->session().setActiveEnv(name.UTF8String);
    [self refreshEnvButton];
    [self toast:[NSString stringWithFormat:StrFmtToastEnv, name]];
}
- (void)refreshEnvButton {
    NSString *active = _apiClient ? N(_apiClient->session().getActiveEnv()) : @"";
    _envButton.title = active.length ? active : kEnvNone;
}

#pragma mark Config screen (ENV + Setting)

// Setting button -> Settings screen; ENV "Manage…" -> Environments screen (2 separate screens).
- (void)settingClicked:(id)sender { [self enterConfig:1]; }

- (void)enterConfig:(NSInteger)kind {
    if (!_apiClient) { [self toastWarn:StrToastOpenFolderFirst]; return; }
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
        core::AppConfig c = _apiClient->appConfig().load();
        _settingEditor.string = [NSString stringWithFormat:
            @"{\n  \"font_name\": \"%s\",\n  \"font_size\": %d,\n"
             "  \"ram_cache_size\": %d,\n  \"disk_cache_size\": %d\n}",
            c.fontName.c_str(), c.fontSize,
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
    if (_apiClient) {
        if (_configKind == 0) {
            [_envVC save];
        } else {
            NSData *d = [_settingEditor.string dataUsingEncoding:NSUTF8StringEncoding];
            NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
            if (dict) {
                core::AppConfig c = _apiClient->appConfig().load();
                if (dict[@"font_name"]) c.fontName = [dict[@"font_name"] UTF8String];
                if (dict[@"font_size"]) c.fontSize = [dict[@"font_size"] intValue];
                if (dict[@"ram_cache_size"]) c.ramCacheSizeMb = [dict[@"ram_cache_size"] intValue];
                if (dict[@"disk_cache_size"]) c.diskCacheSizeMb = [dict[@"disk_cache_size"] intValue];
                _apiClient->appConfig().save(c);
                _apiClient->cache().reloadCacheConfig();   // apply new cap/threshold -> evict if smaller (§1.2)
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
    if ([self requestType] != core::RequestType::Grpc) return;
    if (_protoPopup.selectedIndex == 1) {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        UTType *protoType = [UTType typeWithFilenameExtension:@"proto"];   // .proto descriptor files
        if (protoType) p.allowedContentTypes = @[ protoType ];
        if ([p runModal] == NSModalResponseOK) {
            std::vector<std::string> files{p.URL.lastPathComponent.UTF8String};
            std::vector<std::string> imps{p.URL.URLByDeletingLastPathComponent.path.UTF8String};
            auto ps = core::domain::ProtoSource::files(imps, files);
            if (ps) {
                core::domain::ProtoSource psv = ps.take();
                [self mutateGrpc:^(core::domain::GrpcRequest::Parts &pt) { pt.protoSource = psv; }];
            }
        } else {
            // Cancel file selection -> revert to previous state (reflection).
            _protoPopup.selectedIndex = 0;
            [_protoPopup setNeedsDisplay:YES];
            [self mutateGrpc:^(core::domain::GrpcRequest::Parts &pt) {
                pt.protoSource = core::domain::ProtoSource::reflection();
            }];
        }
    } else {
        [self mutateGrpc:^(core::domain::GrpcRequest::Parts &pt) {
            pt.protoSource = core::domain::ProtoSource::reflection();
        }];
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
    _grpcMethodsFetched = NO;   // invalidate: only ONE request's list is kept; next open re-fetches
    const core::domain::GrpcRequest *g =
        (_model && _model->type() == core::domain::RequestType::Grpc)
            ? &std::get<core::domain::GrpcRequest>(_model->payload())
            : nullptr;
    if (!g || g->service().empty() || g->method().empty()) {
        _servicePopup.itemTitles = @[ StrNoRpc ];
        _servicePopup.toolTip = nil;
    } else {
        NSString *full = [NSString stringWithFormat:@"%s/%s", g->service().c_str(), g->method().c_str()];
        _servicePopup.itemTitles =
            @[ GrpcRpcLabel(g->service(), g->method(), core::domain::toString(g->methodType())) ];
        _servicePopup.toolTip = full;   // hover shows the fully-qualified pkg.Service/Method
    }
    _servicePopup.selectedIndex = 0;
    [_servicePopup setNeedsDisplay:YES];
}

// Proto source / URL changed -> the RPC list is stale (different server or source). Invalidate so the
// NEXT dropdown open re-fetches from scratch; don't hit the network on every edit.
- (void)reloadGrpcMethods { [self showSavedGrpcMethodLabel]; }

// Load the service/RPC list from the current proto source (reflection: query host; .proto: parse).
// openWhenDone = YES: open the menu right after loading (used when clicking the dropdown to pick an RPC).
// Runs in background because reflection does network IO; only applies the result of the latest call.
- (void)fetchGrpcMethodsThenOpen:(BOOL)openWhenDone {
    if (!_model || _model->type() != core::domain::RequestType::Grpc || !_apiClient) return;
    std::string target = _urlField.stringValue.UTF8String; // target = URL field (host:port)
    [self mutateGrpc:^(core::domain::GrpcRequest::Parts &pt) { pt.target = target; }]; // persist edited target
    const auto &g = std::get<core::domain::GrpcRequest>(_model->payload());
    // Reflection needs a host; .proto parses a file so a host is not required.
    bool reflection = g.protoSource().match(
        [](auto &&x) { return std::is_same_v<std::decay_t<decltype(x)>, core::domain::ProtoReflection>; });
    if (reflection && g.target().empty()) {
        _grpcMethods.clear();
        _servicePopup.itemTitles = @[ StrNoRpc ];
        _servicePopup.selectedIndex = 0;
        _servicePopup.toolTip = nil;
        [_servicePopup setNeedsDisplay:YES];
        if (openWhenDone) [self toastWarn:StrToastEnterGrpcHost];
        return;
    }
    _servicePopup.itemTitles = @[ StrFetching ];
    _servicePopup.selectedIndex = 0;
    [_servicePopup setNeedsDisplay:YES];

    uint64_t seq = ++_grpcMethodsReqSeq;
    core::domain::GrpcRequest gReq = g; // copy for the background thread
    core::app::CoreApiClient *api = _apiClient.get();
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto res = api->listGrpcMethods(gReq);
        std::vector<core::domain::GrpcMethodDescriptor> methods =
            res.isOk() ? res.value() : std::vector<core::domain::GrpcMethodDescriptor>{};
        NSString *errStr = res.isOk() ? nil : N(res.error().message);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s = ws;
            if (!s || seq != s->_grpcMethodsReqSeq) return; // a newer request already exists
            [s applyGrpcMethods:methods error:errStr openMenu:openWhenDone];
        });
    });
}

- (void)applyGrpcMethods:(const std::vector<core::domain::GrpcMethodDescriptor> &)methods error:(NSString *)err
                openMenu:(BOOL)openMenu {
    _grpcMethods = methods;
    if (methods.empty()) {
        _grpcMethodsFetched = NO;   // failed/empty -> allow a retry on the next open
        _servicePopup.itemTitles = @[ StrNoRpc ];
        _servicePopup.selectedIndex = 0;
        _servicePopup.toolTip = nil;
        [_servicePopup setNeedsDisplay:YES];
        if (err.length) [self toastWarn:[NSString stringWithFormat:StrFmtToastListRpcs, err]];
        return;
    }
    _grpcMethodsFetched = YES;       // success -> reuse this list; don't re-fetch until invalidated
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    NSInteger sel = 0;
    std::string curSvc, curMethod;   // current selection to preselect in the list
    if (_model && _model->type() == core::domain::RequestType::Grpc) {
        const auto &cg = std::get<core::domain::GrpcRequest>(_model->payload());
        curSvc = cg.service();
        curMethod = cg.method();
    }
    for (size_t i = 0; i < methods.size(); ++i) {
        const core::domain::GrpcMethodDescriptor &m = methods[i];
        [titles addObject:GrpcRpcLabel(m.service, m.method, core::domain::toString(m.type))];
        if (m.service == curSvc && m.method == curMethod) sel = (NSInteger)i;
    }
    _servicePopup.itemTitles = titles;
    _servicePopup.selectedIndex = sel;
    [_servicePopup setNeedsDisplay:YES];
    [self applySelectedGrpcMethod:sel]; // sync model with displayed selection
    if (openMenu) [_servicePopup openMenu];
}

#pragma mark - GraphQL schema introspection (Schema response tab)

// Drop the cached schema (URL edit / request switch / send failure). Silent — no pane/display touch;
// the NEXT Schema-tab click re-fetches. Mirrors showSavedGrpcMethodLabel's invalidate role.
- (void)invalidateGqlSchema {
    _gqlSchemaSdl = nil;
    _gqlSchemaJson = nil;
    _gqlSchemaFetched = NO;
    _gqlSchemaFetching = NO;
}

- (BOOL)respActiveTabIsSchema {
    return [self requestType] == core::RequestType::GraphQl &&
           _activeRespTab >= 0 && _activeRespTab < (NSInteger)_respTabTitles.count &&
           [_respTabTitles[_activeRespTab] isEqualToString:StrTabSchema];
}

// Single render point for the Schema tab: Pretty (and Encode/Decode) -> SDL, Raw -> introspection JSON.
- (void)displayGqlSchemaPane {
    if (_gqlSchemaFetching) { _respText.string = StrFetchingSchema; return; }
    if (!_gqlSchemaFetched) { _respText.string = @""; return; }
    _respText.string = (_prettyMode == 1 ? _gqlSchemaJson : _gqlSchemaSdl) ?: @"";
}

// Fetch the endpoint's schema via the standard introspection query (first Schema-tab click). Mirrors
// fetchGrpcMethodsThenOpen: silent editor sync, seq race guard, background call, main-queue apply.
- (void)fetchGqlSchema {
    if (!_model || _model->type() != core::domain::RequestType::GraphQl || !_apiClient) return;
    // Persist URL/headers/auth edits into the model; on bad-JSON sync failure proceed with the
    // last-good model (same tolerance as autosave).
    [self syncModelFromEditors:YES];
    const auto &g = std::get<core::domain::GraphQlRequest>(_model->payload());
    if (g.url().raw().empty()) {
        [self toastWarn:StrToastEnterGqlUrl];
        return;
    }
    _gqlSchemaFetching = YES;
    if ([self respActiveTabIsSchema]) [self displayGqlSchemaPane]; // "Fetching schema..."

    uint64_t seq = ++_gqlSchemaReqSeq;
    core::domain::RequestModel mReq = *_model; // copy for the background thread
    core::app::CoreApiClient *api = _apiClient.get();
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto res = api->introspectGraphQl(mReq);
        NSString *sdl = res.isOk() ? N(res.value().sdl) : nil;
        NSString *json = res.isOk() ? N(res.value().json) : nil;
        NSString *errStr = res.isOk() ? nil : N(res.error().message);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s = ws;
            if (!s || seq != s->_gqlSchemaReqSeq) return; // a newer fetch (or invalidation) superseded this
            [s applyGqlSchema:sdl json:json error:errStr];
        });
    });
}

- (void)applyGqlSchema:(NSString *)sdl json:(NSString *)json error:(NSString *)err {
    _gqlSchemaFetching = NO;
    if (err.length || !sdl) {
        _gqlSchemaFetched = NO; // failed -> the next click retries
        _gqlSchemaSdl = nil;
        _gqlSchemaJson = nil;
        [self toastWarn:[NSString stringWithFormat:StrFmtToastFetchSchema, err ?: @"unknown error"]];
        if ([self respActiveTabIsSchema])
            _respText.string = [NSString stringWithFormat:@"[Error] %@", err ?: @""];
        return;
    }
    _gqlSchemaSdl = sdl;
    _gqlSchemaJson = json ?: @"";
    _gqlSchemaFetched = YES;
    if ([self respActiveTabIsSchema]) [self displayGqlSchemaPane];
}

// User picks an RPC -> write service/method/methodType into the model (autosave persists it).
- (void)serviceMethodChanged:(id)sender {
    [self applySelectedGrpcMethod:_servicePopup.selectedIndex];
}

- (void)applySelectedGrpcMethod:(NSInteger)idx {
    if (idx < 0 || idx >= (NSInteger)_grpcMethods.size()) return;
    const core::domain::GrpcMethodDescriptor &m = _grpcMethods[(size_t)idx];
    std::string svc = m.service, meth = m.method;
    core::domain::GrpcMethodType mt = m.type;
    [self mutateGrpc:^(core::domain::GrpcRequest::Parts &pt) {
        pt.service = svc;
        pt.method = meth;
        pt.methodType = mt;
    }];
    // Hovering the button shows the full RPC name (the button may have truncated it with "…").
    _servicePopup.toolTip = [NSString stringWithFormat:@"%s/%s", m.service.c_str(), m.method.c_str()];
}

- (void)manageEnv:(id)sender { [self enterConfig:0]; }
@end
