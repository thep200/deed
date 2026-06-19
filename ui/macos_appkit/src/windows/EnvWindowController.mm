#import "windows/EnvWindowController.h"
#import "app/OS9Lifecycle.h"
#import "dialogs/OS9Dialog.h"
#import "widgets/OS9EnvGrid.h"
#import "widgets/OS9Toast.h"

#include <string>
#include <vector>

#include "core/engine.hpp"
#include "core/persistence/stores.hpp"
#include "core/types.hpp"

// Key nội bộ của cột base. UI hiển thị "Local" (SPEC §T4 / Q2) nhưng key vẫn "Global"
// để KHÔNG đổi ngữ nghĩa resolve {{var}} ở core.
static NSString *const kBaseEnv = @"Global";
static NSString *const kBaseLabel = @"Local";

static NSString *Key(NSString *env, NSString *alias) {
    return [NSString stringWithFormat:@"%@\t%@", env, alias];
}

@implementation EnvWindowController {
    core::Engine *_engine; // không sở hữu
    OS9EnvGrid *_grid;

    NSMutableArray<NSString *> *_envNames;  // index 0 = kBaseEnv
    NSMutableArray<NSString *> *_aliases;
    NSMutableDictionary<NSString *, NSString *> *_values; // Key(env,alias) -> value
    NSMutableSet<NSString *> *_dirtyEnvs;
    NSMutableSet<NSString *> *_removedEnvs; // env cần xoá file lúc save (rename/delete)
}

- (instancetype)initWithEngine:(core::Engine *)engine {
    if ((self = [super init])) {
        _engine = engine;
        _envNames = [NSMutableArray array];
        _aliases = [NSMutableArray array];
        _values = [NSMutableDictionary dictionary];
        _dirtyEnvs = [NSMutableSet set];
        _removedEnvs = [NSMutableSet set];
    }
    return self;
}

- (NSView *)view {
    if (_grid) return _grid;
    _grid = [[OS9EnvGrid alloc] initWithFrame:NSMakeRect(0, 0, 700, 360)];
    _grid.delegate = self;
    _grid.baseDisplayName = kBaseLabel;
    return _grid;
}

- (void)layout { [_grid layout]; }

- (void)pushToGrid {
    _grid.envNames = _envNames;
    _grid.aliases = _aliases;
    [_grid reloadData];
}

- (void)reload {
    OS9SafeEndEditing(_grid.window, nil);
    [self view];
    [self loadFromStore];
    [self pushToGrid];
}

- (void)loadFromStore {
    [_envNames removeAllObjects];
    [_aliases removeAllObjects];
    [_values removeAllObjects];
    [_dirtyEnvs removeAllObjects];
    [_removedEnvs removeAllObjects];
    if (!_engine) return;

    [_envNames addObject:kBaseEnv];
    for (const auto &n : _engine->environments().list())
        if (n != kBaseEnv.UTF8String) [_envNames addObject:[NSString stringWithUTF8String:n.c_str()]];

    NSMutableArray<NSString *> *aliasOrder = [NSMutableArray array];
    for (NSString *env in _envNames) {
        core::Environment e;
        try { e = _engine->environments().load(env.UTF8String); }
        catch (...) { continue; }
        for (const auto &k : e.keys) {
            NSString *alias = [NSString stringWithUTF8String:k.key.c_str()];
            if (![aliasOrder containsObject:alias]) [aliasOrder addObject:alias];
            _values[Key(env, alias)] = [NSString stringWithUTF8String:k.value.c_str()];
        }
    }
    [_aliases addObjectsFromArray:aliasOrder];
}

- (void)save {
    if (!_engine) return;
    OS9SafeEndEditing(_grid.window, nil);
    [_grid commitEditing];
    for (NSString *env in _dirtyEnvs) {
        if ([_removedEnvs containsObject:env]) continue;
        core::Environment e;
        e.name = env.UTF8String;
        for (NSString *alias in _aliases) {
            core::EnvKey k;
            k.key = alias.UTF8String;
            k.enabled = true;
            k.value = (_values[Key(env, alias)] ?: @"").UTF8String;
            e.keys.push_back(k);
        }
        try { _engine->environments().save(e); } catch (...) {}
    }
    for (NSString *env in _removedEnvs) {
        try { _engine->environments().remove(env.UTF8String); } catch (...) {}
    }
    [_dirtyEnvs removeAllObjects];
    [_removedEnvs removeAllObjects];
}

#pragma mark OS9EnvGridDelegate

- (NSString *)envGrid:(OS9EnvGrid *)g valueForAlias:(NSString *)alias env:(NSString *)env {
    return _values[Key(env, alias)] ?: @"";
}

- (void)envGrid:(OS9EnvGrid *)g setValue:(NSString *)val forAlias:(NSString *)alias env:(NSString *)env {
    _values[Key(env, alias)] = val;
    [_dirtyEnvs addObject:env];
    [_removedEnvs removeObject:env];
    [g reloadData];
}

- (void)envGrid:(OS9EnvGrid *)g renameAlias:(NSString *)oldAlias to:(NSString *)newAlias {
    if ([_aliases containsObject:newAlias]) {
        [self errorDialog:[NSString stringWithFormat:@"Alias \"%@\" đã tồn tại.", newAlias]];
        return;
    }
    for (NSString *env in _envNames) {
        NSString *ov = _values[Key(env, oldAlias)];
        if (ov != nil) {
            _values[Key(env, newAlias)] = ov;
            [_values removeObjectForKey:Key(env, oldAlias)];
            [_dirtyEnvs addObject:env];
        }
    }
    NSInteger idx = [_aliases indexOfObject:oldAlias];
    if (idx != NSNotFound) _aliases[idx] = newAlias;
    [self pushToGrid];
    [self warnVarRename:oldAlias];
}

- (void)envGrid:(OS9EnvGrid *)g renameEnv:(NSString *)oldEnv to:(NSString *)newEnv {
    if ([oldEnv isEqualToString:kBaseEnv]) return;   // base không đổi tên
    if ([_envNames containsObject:newEnv]) {
        [self errorDialog:[NSString stringWithFormat:@"Environment \"%@\" đã tồn tại.", newEnv]];
        return;
    }
    for (NSString *alias in _aliases) {
        NSString *ov = _values[Key(oldEnv, alias)];
        if (ov != nil) {
            _values[Key(newEnv, alias)] = ov;
            [_values removeObjectForKey:Key(oldEnv, alias)];
        }
    }
    NSInteger idx = [_envNames indexOfObject:oldEnv];
    if (idx != NSNotFound) _envNames[idx] = newEnv;
    [_dirtyEnvs addObject:newEnv];
    [_removedEnvs addObject:oldEnv];   // xoá file cũ khi save
    // Cập nhật activeEnv nếu trùng.
    if (_engine && _engine->session().getActiveEnv() == std::string(oldEnv.UTF8String))
        _engine->session().setActiveEnv(newEnv.UTF8String);
    [self pushToGrid];
}

- (void)envGrid:(OS9EnvGrid *)g addEnvNamed:(NSString *)name {
    if (!name.length || [_envNames containsObject:name]) return;   // grid đã validate; thủ thêm lần nữa
    [_envNames addObject:name];
    [_dirtyEnvs addObject:name];
    [_removedEnvs removeObject:name];
    for (NSString *alias in _aliases) _values[Key(name, alias)] = @"";
    [self pushToGrid];
}

- (void)envGrid:(OS9EnvGrid *)g deleteEnv:(NSString *)env {
    if ([env isEqualToString:kBaseEnv]) return;
    NSInteger r = [OS9Dialog confirmWithTitle:@"Xoá environment"
                                      message:[NSString stringWithFormat:@"Xoá env \"%@\"? Mọi giá trị trong cột sẽ mất.", env]
                                      buttons:@[ @"Cancel", @"Xoá" ]
                                defaultButton:1 cancelButton:0 icon:OS9AlertCaution
                                       parent:_grid.window];
    if (r != 1) return;
    for (NSString *alias in _aliases) [_values removeObjectForKey:Key(env, alias)];
    [_envNames removeObject:env];
    [_dirtyEnvs removeObject:env];
    [_removedEnvs addObject:env];
    if (_engine && _engine->session().getActiveEnv() == std::string(env.UTF8String))
        _engine->session().setActiveEnv(kBaseEnv.UTF8String);   // reset về base
    [self pushToGrid];
}

- (void)envGrid:(OS9EnvGrid *)g addAliasNamed:(NSString *)name {
    if (!name.length || [_aliases containsObject:name]) return;
    [_aliases addObject:name];
    for (NSString *env in _envNames) { _values[Key(env, name)] = @""; [_dirtyEnvs addObject:env]; }
    [self pushToGrid];
}

- (void)envGrid:(OS9EnvGrid *)g deleteAlias:(NSString *)alias {
    NSInteger r = [OS9Dialog confirmWithTitle:@"Xoá alias"
                                      message:[NSString stringWithFormat:@"Xoá alias \"%@\"? Giá trị trên mọi env sẽ mất.", alias]
                                      buttons:@[ @"Cancel", @"Xoá" ]
                                defaultButton:1 cancelButton:0 icon:OS9AlertCaution
                                       parent:_grid.window];
    if (r != 1) return;
    for (NSString *env in _envNames) { [_values removeObjectForKey:Key(env, alias)]; [_dirtyEnvs addObject:env]; }
    [_aliases removeObject:alias];
    [self pushToGrid];
}

#pragma mark helpers

- (void)errorDialog:(NSString *)msg {
    [OS9Dialog confirmWithTitle:@"Không hợp lệ" message:msg
                        buttons:@[ @"OK" ] defaultButton:0 cancelButton:-1
                           icon:OS9AlertStop parent:_grid.window];
}

- (void)warnVarRename:(NSString *)oldAlias {
    // SPEC §T3: đổi tên alias KHÔNG tự sửa {{old}} trong request đã lưu -> cảnh báo.
    NSWindow *win = _grid.window;
    if (!win) return;
    NSString *msg = [NSString stringWithFormat:@"Đã đổi tên biến; request dùng {{%@}} cần cập nhật thủ công.", oldAlias];
    OS9Toast *t = [[OS9Toast alloc] initWithMessage:msg kind:0];
    NSSize sz = [OS9Toast sizeForMessage:msg];
    NSView *cv = win.contentView;
    t.frame = NSMakeRect((cv.bounds.size.width - sz.width) / 2, 24, sz.width, sz.height);
    t.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMaxYMargin;
    __weak OS9Toast *wt = t;
    t.onClose = ^{ [wt removeFromSuperview]; };
    [cv addSubview:t];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [wt removeFromSuperview];
    });
}

@end
