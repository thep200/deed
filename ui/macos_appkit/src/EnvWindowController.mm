#import "EnvWindowController.h"

#import "OS9Theme.h"
#import "OS9Widgets.h"

#include <string>
#include <vector>

#include "core/engine.hpp"
#include "core/stores.hpp"
#include "core/types.hpp"

static NSString *Key(NSString *env, NSString *alias) {
    return [NSString stringWithFormat:@"%@\t%@", env, alias];
}

@interface EnvFlipped : NSView
@end
@implementation EnvFlipped
- (BOOL)isFlipped { return YES; }
@end

@implementation EnvWindowController {
    core::Engine *_engine; // không sở hữu
    NSView *_container;
    NSView *_delStrip;      // hàng nút ✕ xoá env (trừ Global)
    NSView *_btnRow;        // hàng +New/+Alias/-Alias/ToggleSecret
    NSTableView *_table;
    NSScrollView *_scroll;

    NSMutableArray<NSString *> *_envNames;      // Global đầu tiên
    NSMutableArray<NSString *> *_aliases;
    NSMutableArray<NSNumber *> *_secret;        // per alias
    NSMutableDictionary<NSString *, NSString *> *_values; // Key(env,alias) -> value
    NSMutableSet<NSString *> *_dirtyEnvs;
}

- (instancetype)initWithEngine:(core::Engine *)engine {
    if ((self = [super init])) {
        _engine = engine;
        _envNames = [NSMutableArray array];
        _aliases = [NSMutableArray array];
        _secret = [NSMutableArray array];
        _values = [NSMutableDictionary dictionary];
        _dirtyEnvs = [NSMutableSet set];
    }
    return self;
}

- (NSView *)view {
    if (_container) return _container;

    _container = [[EnvFlipped alloc] initWithFrame:NSMakeRect(0, 0, 700, 360)];

    _delStrip = [[EnvFlipped alloc] initWithFrame:NSMakeRect(0, 0, 700, 24)];
    [_container addSubview:_delStrip];

    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 28, 700, 300)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.scrollerStyle = NSScrollerStyleOverlay;
    _scroll.borderType = NSBezelBorder;
    _table = [[NSTableView alloc] initWithFrame:_scroll.bounds];
    _table.dataSource = self;
    _table.delegate = self;
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.allowsMultipleSelection = YES;
    _table.font = [OS9Theme uiFont];
    _scroll.documentView = _table;
    [_container addSubview:_scroll];

    _btnRow = [[EnvFlipped alloc] initWithFrame:NSMakeRect(0, 332, 700, 26)];
    OS9BevelButton *newEnv = [[OS9BevelButton alloc] initWithTitle:@"+ New Env" target:self action:@selector(addEnv:)];
    newEnv.frame = NSMakeRect(0, 0, 90, 24);
    OS9BevelButton *addAlias = [[OS9BevelButton alloc] initWithTitle:@"+ Alias" target:self action:@selector(addAlias:)];
    addAlias.frame = NSMakeRect(96, 0, 80, 24);
    OS9BevelButton *delAlias = [[OS9BevelButton alloc] initWithTitle:@"– Alias" target:self action:@selector(delAlias:)];
    delAlias.frame = NSMakeRect(182, 0, 80, 24);
    OS9BevelButton *toggleSecret = [[OS9BevelButton alloc] initWithTitle:@"Toggle Secret" target:self action:@selector(toggleSecret:)];
    toggleSecret.frame = NSMakeRect(268, 0, 110, 24);
    for (NSView *v in @[ newEnv, addAlias, delAlias, toggleSecret ]) [_btnRow addSubview:v];
    [_container addSubview:_btnRow];

    return _container;
}

- (void)layout {
    if (!_container) return;
    CGFloat W = _container.bounds.size.width, H = _container.bounds.size.height;
    _delStrip.frame = NSMakeRect(0, 0, W, 24);
    _btnRow.frame = NSMakeRect(0, H - 26, W, 26);
    _scroll.frame = NSMakeRect(0, 28, W, H - 28 - 30);
}

// Dựng lại hàng nút ✕ xoá env (mỗi env thường một nút; Global không có).
- (void)rebuildDeleteStrip {
    for (NSView *v in [_delStrip.subviews copy]) [v removeFromSuperview];
    CGFloat x = 0;
    for (NSString *env in _envNames) {
        if ([env isEqualToString:@"Global"]) continue;
        OS9BevelButton *b = [[OS9BevelButton alloc] initWithTitle:[NSString stringWithFormat:@"✕ %@", env]
                                                           target:self action:@selector(deleteEnvClicked:)];
        b.frame = NSMakeRect(x, 0, 96, 22);
        b.identifier = env;
        [_delStrip addSubview:b];
        x += 100;
    }
}

- (void)deleteEnvClicked:(OS9BevelButton *)sender {
    NSString *env = sender.identifier;
    if (!env || [env isEqualToString:@"Global"]) return;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Xoá môi trường \"%@\"?", env];
    [a addButtonWithTitle:@"Delete"]; [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
    try { _engine->environments().remove(env.UTF8String); } catch (...) {}
    [self reload];
}

- (void)reload {
    [self view];
    [self loadFromStore];
    [self rebuildColumns];
    [self rebuildDeleteStrip];
    [self layout];
    [_table reloadData];
}

- (void)loadFromStore {
    [_envNames removeAllObjects];
    [_aliases removeAllObjects];
    [_secret removeAllObjects];
    [_values removeAllObjects];
    [_dirtyEnvs removeAllObjects];

    [_envNames addObject:@"Global"];
    for (const auto &n : _engine->environments().list())
        if (n != "Global") [_envNames addObject:[NSString stringWithUTF8String:n.c_str()]];

    NSMutableArray<NSString *> *aliasOrder = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSNumber *> *secretByAlias = [NSMutableDictionary dictionary];
    for (NSString *env in _envNames) {
        core::Environment e;
        try { e = _engine->environments().load(env.UTF8String); }
        catch (...) { continue; }
        for (const auto &k : e.keys) {
            NSString *alias = [NSString stringWithUTF8String:k.key.c_str()];
            if (![aliasOrder containsObject:alias]) [aliasOrder addObject:alias];
            if (k.secret) secretByAlias[alias] = @YES;
            _values[Key(env, alias)] = [NSString stringWithUTF8String:k.value.c_str()];
        }
    }
    for (NSString *a in aliasOrder) {
        [_aliases addObject:a];
        [_secret addObject:secretByAlias[a] ?: @NO];
    }
}

- (void)rebuildColumns {
    while (_table.tableColumns.count) [_table removeTableColumn:_table.tableColumns.lastObject];
    NSTableColumn *aliasCol = [[NSTableColumn alloc] initWithIdentifier:@"__alias__"];
    aliasCol.title = @"Alias";
    aliasCol.width = 160;
    [_table addTableColumn:aliasCol];
    for (NSString *env in _envNames) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:env];
        c.title = [env isEqualToString:@"Global"] ? @"Global 🔒" : env;
        c.width = 130;
        [_table addTableColumn:c];
    }
}

#pragma mark Data source (cell-based)

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return _aliases.count; }

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    BOOL isSecret = _secret[row].boolValue;
    if ([col.identifier isEqualToString:@"__alias__"])
        return [NSString stringWithFormat:@"%@%@", isSecret ? @"🔒 " : @"", _aliases[row]];
    NSString *v = _values[Key(col.identifier, _aliases[row])] ?: @"";
    if (isSecret && v.length) return @"••••";
    return v;
}

- (void)tableView:(NSTableView *)tv setObjectValue:(id)obj forTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    NSString *val = [obj description];
    if ([col.identifier isEqualToString:@"__alias__"]) {
        NSString *clean = [val stringByReplacingOccurrencesOfString:@"🔒 " withString:@""];
        NSString *old = _aliases[row];
        if ([clean isEqualToString:old] || clean.length == 0) return;
        for (NSString *env in _envNames) {
            NSString *ov = _values[Key(env, old)];
            if (ov) { _values[Key(env, clean)] = ov; [_values removeObjectForKey:Key(env, old)]; [_dirtyEnvs addObject:env]; }
        }
        _aliases[row] = clean;
        return;
    }
    if ([val isEqualToString:@"••••"]) return;
    _values[Key(col.identifier, _aliases[row])] = val;
    [_dirtyEnvs addObject:col.identifier];
}

- (void)tableView:(NSTableView *)tv willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    if ([cell respondsToSelector:@selector(setFont:)]) [cell setFont:[OS9Theme uiFont]];
}

#pragma mark Actions

- (void)addEnv:(id)sender {
    NSString *name = [self prompt:@"Tên môi trường mới" def:@"Dev"];
    if (!name || name.length == 0) return;
    if ([_envNames containsObject:name]) { NSBeep(); return; }
    [_envNames addObject:name];
    [_dirtyEnvs addObject:name];
    [self rebuildColumns];
    [_table reloadData];
}

- (void)addAlias:(id)sender {
    NSString *name = [self prompt:@"Tên alias mới" def:@"baseUrl"];
    if (!name || name.length == 0) return;
    if ([_aliases containsObject:name]) { NSBeep(); return; }
    [_aliases addObject:name];
    [_secret addObject:@NO];
    for (NSString *env in _envNames) { _values[Key(env, name)] = @""; [_dirtyEnvs addObject:env]; }
    [_table reloadData];
}

- (void)delAlias:(id)sender {
    NSIndexSet *sel = _table.selectedRowIndexes;
    if (sel.count == 0) return;
    [[sel mutableCopy] enumerateIndexesWithOptions:NSEnumerationReverse usingBlock:^(NSUInteger idx, BOOL *stop) {
        NSString *alias = _aliases[idx];
        for (NSString *env in _envNames) { [_values removeObjectForKey:Key(env, alias)]; [_dirtyEnvs addObject:env]; }
        [_aliases removeObjectAtIndex:idx];
        [_secret removeObjectAtIndex:idx];
    }];
    [_table reloadData];
}

- (void)toggleSecret:(id)sender {
    NSInteger row = _table.selectedRow;
    if (row < 0) return;
    _secret[row] = @(!_secret[row].boolValue);
    for (NSString *env in _envNames) [_dirtyEnvs addObject:env];
    [_table reloadData];
}

- (void)save {
    for (NSString *env in _dirtyEnvs) {
        core::Environment e;
        e.name = env.UTF8String;
        for (NSUInteger i = 0; i < _aliases.count; i++) {
            core::EnvKey k;
            k.key = _aliases[i].UTF8String;
            k.secret = _secret[i].boolValue;
            k.enabled = true;
            k.value = (_values[Key(env, _aliases[i])] ?: @"").UTF8String;
            e.keys.push_back(k);
        }
        try { _engine->environments().save(e); } catch (...) {}
    }
    [_dirtyEnvs removeAllObjects];
}

- (NSString *)prompt:(NSString *)title def:(NSString *)def {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = title;
    NSTextField *tf = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 220, 24)];
    tf.stringValue = def ?: @"";
    a.accessoryView = tf;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] == NSAlertFirstButtonReturn) return tf.stringValue;
    return nil;
}

@end
