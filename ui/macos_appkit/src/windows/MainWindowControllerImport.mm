#import "windows/MainWindowControllerPrivate.h"

@implementation MainWindowController (Import)

// Import-kind facts (exhaustive switches — no default: a new ImportKind must be handled here).
static NSString *ImportKindLabel(core::domain::ImportKind k) {
    switch (k) {
    case core::domain::ImportKind::Curl: return @"cURL";
    case core::domain::ImportKind::Grpcurl: return @"grpcurl";
    case core::domain::ImportKind::GraphQl: return @"GraphQL";
    case core::domain::ImportKind::Ldap: return @"ldapsearch";
    }
    return @"cURL";
}
static NSString *ImportKindDetected(core::domain::ImportKind k) {
    switch (k) {
    case core::domain::ImportKind::Curl: return StrCurlDetected;
    case core::domain::ImportKind::Grpcurl: return StrGrpcurlDetected;
    case core::domain::ImportKind::GraphQl: return StrGraphqlDetected;
    case core::domain::ImportKind::Ldap: return StrCurlDetected; // ldapsearch auto-imports; dialog unreachable
    }
    return StrCurlDetected;
}

// Run the importer via IImportService (CoreApiClient) — returns a DOMAIN RequestModel inside
// core::ImportParseResult. Pure + thread-safe -> safe to call from the background import queue.
static core::ImportParseResult GqlRunImport(MainWindowController *self, core::domain::ImportKind kind,
                                            const char *t) {
    core::ImportParseResult out;
    if (!self->_apiClient) { out.ok = false; out.error = "import unavailable"; return out; }
    auto r = self->_apiClient->importText(t, kind);
    if (!r.isOk()) { out.ok = false; out.error = r.error().message; return out; }
    out.ok = true;
    out.model = r.value().model;
    out.unknown = r.value().unknown;
    return out;
}

// Import + create request immediately, no prompt; report result via toast.
// Parse (possibly large) in BACKGROUND -> doesn't block main; marshal result to main.
- (void)importNow:(NSString *)text kind:(core::domain::ImportKind)kind {
    if (!_apiClient) return;
    NSString *t = [text copy];
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws; if (!s || !s->_apiClient) return;
        __block core::ImportParseResult rb = GqlRunImport(s, kind, t.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws; if (!s2) return;
            if (!rb.ok || !rb.model) {
                [s2 toastWarn:[NSString stringWithFormat:StrFmtToastImportFailed,
                               ImportKindLabel(kind), rb.error.c_str()]];
                [s2 restoreUrlField];
                return;
            }
            [s2 applyImport:*rb.model];
        });
    });
}

// Show a confirmation preview; if OK -> create a new request in the tree + open the editor.
// Parse in BACKGROUND; dialog + applyImport on main.
- (void)offerImport:(NSString *)text kind:(core::domain::ImportKind)kind {
    if (!_apiClient) return;
    NSString *t = [text copy];
    __weak MainWindowController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MainWindowController *s = ws; if (!s || !s->_apiClient) return;
        __block core::ImportParseResult rb = GqlRunImport(s, kind, t.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            MainWindowController *s2 = ws; if (!s2) return;
            if (!rb.ok || !rb.model) {
                [s2 toastWarn:[NSString stringWithFormat:StrFmtToastImportFailed,
                               ImportKindLabel(kind), rb.error.c_str()]];
                return;
            }
            NSString *primary = s2->_hasRequest ? StrBtnReplaceCurrent : StrBtnCreateRequest;
            NSMutableString *summary =
                [[TypeUiFor(rb.model->type()) importSummary:*rb.model] mutableCopy];
            if (!rb.unknown.empty()) {
                [summary appendString:@"\nskipped:"];
                for (const auto &u : rb.unknown) [summary appendFormat:@" %s", u.c_str()];
            }
            NSString *body = [NSString stringWithFormat:@"%@\n\n%@", ImportKindDetected(kind), summary];
            NSInteger choice = [OS9Dialog confirmWithTitle:StrDlgImportTitle
                                                   message:body
                                                   buttons:@[ StrCancel, primary ]
                                             defaultButton:1 cancelButton:0
                                                      icon:OS9AlertNote
                                                    parent:s2->_window];
            if (choice == 1) [s2 applyImport:*rb.model];
            else [s2 restoreUrlField];   // cancel: restore the URL field to the open request's value
        });
    });
}

// REPLACE the open request with the imported model (keep id/name/file, swap type + payload).
// No request open -> create new (fallback).
- (void)applyImport:(const core::domain::RequestModel &)rawModel {
    namespace d = core::domain;
    // Proactively rewrite literal values matching the active env back to {{alias}} on import.
    d::RequestModel m = _apiClient ? _apiClient->aliasifyModel(rawModel) : rawModel;
    if (!_hasRequest || _currentRel.empty() || ![self resyncCurrentRelById]) {
        NSString *name = [TypeUiFor(m.type()) importedName:m];   // fallback: no request open -> create new
        try {
            std::string folderRel = [self selectedFolderRel];   // refresh only the target level, not the whole tree
            std::string rel = _apiClient->collection().createRequestFromModel(folderRel, m, name.UTF8String);
            [self refreshTreeLevel:N(folderRel)];
            [self loadRequestAtRel:N(rel)];
            [self toastOk:[NSString stringWithFormat:StrFmtToastImportedCreated, name]];
        } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
        return;
    }
    if (!_model) return;
    // Replace in place: keep the current request's id + name + seq; take the imported payload + config.
    d::RequestModel n =
        d::RequestModel::create(_model->id(), _model->name(), _model->seq(), m.config(), m.payload()).take();
    _model = n;
    _hasResp = NO;
    [self setRequestType:n.type()];      // rebuild tabs for the new type (http <-> grpc)
    [self populateEditorsFromModel];
    [self setHasRequest:YES];
    _respText.string = @"";              // clear old response
    [self updateTitle];
    [self relayout];
    try {
        _currentRel = _apiClient->collection().saveRequest(_currentRel, *_model, [self collectBodyDrafts]);   // save + sync filename
        _apiClient->session().saveLastOpened(_currentRel);
        NSString *parentRel = [N(_currentRel) stringByDeletingLastPathComponent];  // only the request's containing level
        [self refreshTreeLevel:parentRel];
        [self reselectTreeByRel:N(_currentRel)];
        [self toastOk:[NSString stringWithFormat:StrFmtToastReplaced,
                       TypeUiFor(n.type()).displayName]];
    } catch (const std::exception &e) { [self toastWarn:N(e.what())]; }
}

// Restore the URL field to the open request's URL/target (avoid saving the command text by mistake).
- (void)restoreUrlField {
    if (!_hasRequest || !_model) { _urlField.stringValue = @""; _urlPrevLen = 0; return; }
    NSString *u = [TypeUiFor(_model->type()) urlFieldText:*_model];
    _urlField.stringValue = u;
    _urlPrevLen = u.length;
}

- (void)methodChanged:(id)sender { }
- (void)urlCommitted:(id)sender {
    // gRPC: URL field = target -> Enter reloads the RPC list. GraphQL: endpoint changed -> schema stale.
    // HTTP: split out the query.
    if ([self requestType] == core::RequestType::Grpc) [self reloadGrpcMethods];
    else if ([self requestType] == core::RequestType::GraphQl) [self invalidateGqlSchema];
    else [self parseUrlQueryIntoQueryTab];
}

// Decode one query component: '+' -> space, %XX -> byte (matches Core urlutil::urlDecode).
- (NSString *)urlDecodeComponent:(NSString *)s {
    NSString *plus = [s stringByReplacingOccurrencesOfString:@"+" withString:@" "];
    return [plus stringByRemovingPercentEncoding] ?: plus;
}

// If the URL field has '?...': split the query (decode) -> append to the Query tab, URL keeps raw.
// Used when the user types a query into the URL then Enter/Send (matches cURL import behavior).
- (void)parseUrlQueryIntoQueryTab {
    NSInteger qi = [_reqTabTitles indexOfObject:StrTabQuery];
    if (qi == NSNotFound || qi >= (NSInteger)_reqBuffers.count) return;   // gRPC: no Query tab
    NSString *u = _urlField.stringValue ?: @"";
    NSRange qr = [u rangeOfString:@"?"];
    if (qr.location == NSNotFound) return;                                // no query
    NSString *raw = [u substringToIndex:qr.location];
    NSString *query = [u substringFromIndex:qr.location + 1];
    NSRange hr = [query rangeOfString:@"#"];
    if (hr.location != NSNotFound) query = [query substringToIndex:hr.location];

    [self stashActiveReqBuffer];   // make the current Query tab buffer up to date before appending

    // Take the existing entries in the Query tab (JSON array) then append the parsed ones.
    NSMutableArray *items = [NSMutableArray array];
    NSData *cur = [(_reqBuffers[qi] ?: @"[]") dataUsingEncoding:NSUTF8StringEncoding];
    id arr = cur ? [NSJSONSerialization JSONObjectWithData:cur options:0 error:nil] : nil;
    if ([arr isKindOfClass:[NSArray class]]) [items addObjectsFromArray:arr];
    for (NSString *seg in [query componentsSeparatedByString:@"&"]) {
        if (!seg.length) continue;
        NSRange eq = [seg rangeOfString:@"="];
        NSString *k = (eq.location == NSNotFound) ? seg : [seg substringToIndex:eq.location];
        NSString *v = (eq.location == NSNotFound) ? @"" : [seg substringFromIndex:eq.location + 1];
        NSString *dk = [self urlDecodeComponent:k];
        NSString *dv = [self urlDecodeComponent:v];
        // Same key already in the Query tab -> OVERWRITE with the new value (no duplicate row).
        NSUInteger hit = NSNotFound;
        for (NSUInteger idx = 0; idx < items.count; idx++) {
            id it = items[idx];
            if ([it isKindOfClass:[NSDictionary class]] && [[it objectForKey:@"key"] isEqual:dk]) { hit = idx; break; }
        }
        if (hit != NSNotFound) {
            NSMutableDictionary *md = [items[hit] mutableCopy];
            md[@"value"] = dv;
            md[@"enabled"] = @YES;
            items[hit] = md;
        } else {
            [items addObject:@{@"key" : dk, @"value" : dv, @"enabled" : @YES}];
        }
    }
    NSData *out = [NSJSONSerialization dataWithJSONObject:items options:NSJSONWritingPrettyPrinted error:nil];
    if (out) _reqBuffers[qi] = [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding];

    _urlField.stringValue = raw; _urlPrevLen = raw.length;       // URL field keeps raw
    if (_activeReqTab == qi) _reqText.string = _reqBuffers[qi];   // viewing the Query tab -> refresh
}

- (void)updateTitle {
    // Title bar is CONTEXTUAL: config screen -> "Settings"/"Environments"; otherwise -> request name.
    if (_configMode) {
        _titleBar.title = (_configKind == 0) ? StrTitleEnvironments : StrTitleSettings;
    } else {
        _titleBar.title = (_hasRequest && _model) ? N(_model->name()) : @"";
    }
    [_titleBar setNeedsDisplay:YES];
}

@end
