#import "windows/EnvWindowController.h"
#import "app/AppStrings.h"
#import "app/OS9Lifecycle.h"
#import "dialogs/OS9Dialog.h"
#import "widgets/OS9EnvGrid.h"
#import "widgets/OS9Toast.h"

#include <string>
#include <vector>

#include "core/app/persistence_repositories.hpp" // IEnvironmentRepository/ISessionRepository + Environment/EnvKey

// Col 0 = reserved "Global" base: editable here, hidden from dropdown, always merged under active env,
// protected (no delete/rename). Other envs = normal deletable columns (name == file).
static NSString *Key(NSString *env, NSString *alias) {
    return [NSString stringWithFormat:@"%@\t%@", env, alias];
}
static NSString *GlobalEnvName(void) { return @(core::kGlobalEnvName); }

@implementation EnvWindowController {
    core::app::IEnvironmentRepository *_envRepo;     // not owned (lives in CoreApiClient)
    core::app::ISessionRepository *_sessionRepo;     // not owned
    OS9EnvGrid *_grid;

    NSMutableArray<NSString *> *_envNames;  // index 0 = reserved "Global" (pinned, protected)
    NSMutableArray<NSString *> *_aliases;
    NSMutableDictionary<NSString *, NSString *> *_values; // Key(env,alias) -> value
    NSMutableSet<NSString *> *_dirtyEnvs;
    NSMutableSet<NSString *> *_removedEnvs; // envs whose file to delete on save (rename/delete)
    NSMutableSet<NSString *> *_secretAliases; // aliases marked "secret" (per-alias flag)
}

- (instancetype)initWithEnvRepo:(core::app::IEnvironmentRepository *)envRepo
                        session:(core::app::ISessionRepository *)sessionRepo {
    if ((self = [super init])) {
        _envRepo = envRepo;
        _sessionRepo = sessionRepo;
        _envNames = [NSMutableArray array];
        _aliases = [NSMutableArray array];
        _values = [NSMutableDictionary dictionary];
        _dirtyEnvs = [NSMutableSet set];
        _removedEnvs = [NSMutableSet set];
        _secretAliases = [NSMutableSet set];
    }
    return self;
}

- (NSView *)view {
    if (_grid) return _grid;
    _grid = [[OS9EnvGrid alloc] initWithFrame:NSMakeRect(0, 0, 700, 360)];
    _grid.delegate = self;
    _grid.protectedFirstColumn = YES;   // col 0 = reserved "Global"
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
    [_secretAliases removeAllObjects];
    if (!_envRepo) return;

    // Global pinned first (list() hides it); file created lazily on first dirty save.
    [_envNames addObject:GlobalEnvName()];
    for (const auto &n : _envRepo->list())
        [_envNames addObject:[NSString stringWithUTF8String:n.c_str()]];

    NSMutableArray<NSString *> *aliasOrder = [NSMutableArray array];
    for (NSString *env in _envNames) {
        core::Environment e;
        try { e = _envRepo->load(env.UTF8String); }
        catch (...) { continue; }
        for (const auto &k : e.keys) {
            NSString *alias = [NSString stringWithUTF8String:k.key.c_str()];
            if (![aliasOrder containsObject:alias]) [aliasOrder addObject:alias];
            _values[Key(env, alias)] = [NSString stringWithUTF8String:k.value.c_str()];
            if (k.secret) [_secretAliases addObject:alias];   // secret if any env marks it
        }
    }
    [_aliases addObjectsFromArray:aliasOrder];
}

- (void)save {
    if (!_envRepo) return;
    OS9SafeEndEditing(_grid.window, nil);
    [_grid commitEditing];
    for (NSString *env in _dirtyEnvs) {
        if ([_removedEnvs containsObject:env]) continue;
        core::Environment e;
        e.name = env.UTF8String;
        for (NSString *alias in _aliases) {
            core::EnvKey k;
            k.key = alias.uppercaseString.UTF8String;   // variables always stored UPPER
            k.enabled = true;
            k.value = (_values[Key(env, alias)] ?: @"").UTF8String;
            k.secret = [_secretAliases containsObject:alias];   // per-alias flag, mirrored into every env
            e.keys.push_back(k);
        }
        try { _envRepo->save(e); } catch (...) {}
    }
    for (NSString *env in _removedEnvs) {
        try { _envRepo->remove(env.UTF8String); } catch (...) {}
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

- (BOOL)envGrid:(OS9EnvGrid *)g isSecretForAlias:(NSString *)alias {
    return [_secretAliases containsObject:alias];
}

- (void)envGrid:(OS9EnvGrid *)g setSecret:(BOOL)secret forAlias:(NSString *)alias {
    if (secret) [_secretAliases addObject:alias];
    else [_secretAliases removeObject:alias];
    // Flag lives in every env's copy of this key -> mark all (non-removed) envs dirty.
    for (NSString *env in _envNames)
        if (![_removedEnvs containsObject:env]) [_dirtyEnvs addObject:env];
    [self save];   // enc/de-enc this alias NOW, not on Back
}

- (void)envGrid:(OS9EnvGrid *)g renameAlias:(NSString *)oldAlias to:(NSString *)newAlias {
    newAlias = [newAlias uppercaseString];               // variables always stored UPPER
    if ([newAlias isEqualToString:oldAlias]) return;     // uppercased matches old name -> no change
    if ([_aliases containsObject:newAlias]) {
        [self errorDialog:[NSString stringWithFormat:StrFmtAliasExists, newAlias]];
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
    if ([_secretAliases containsObject:oldAlias]) {   // carry the secret flag to the new name
        [_secretAliases removeObject:oldAlias];
        [_secretAliases addObject:newAlias];
    }
    [self pushToGrid];
    [self warnVarRename:oldAlias];
}

- (void)envGrid:(OS9EnvGrid *)g renameEnv:(NSString *)oldEnv to:(NSString *)newEnv {
    if ([oldEnv isEqualToString:GlobalEnvName()]) return;   // reserved base
    if ([_envNames containsObject:newEnv]) {
        [self errorDialog:[NSString stringWithFormat:StrFmtEnvExists, newEnv]];
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
    [_removedEnvs addObject:oldEnv];   // delete old file on save
    // Update activeEnv if it matches.
    if (_sessionRepo && _sessionRepo->getActiveEnv() == std::string(oldEnv.UTF8String))
        _sessionRepo->setActiveEnv(newEnv.UTF8String);
    [self pushToGrid];
}

- (void)envGrid:(OS9EnvGrid *)g addEnvNamed:(NSString *)name {
    if (!name.length || [_envNames containsObject:name]) return;   // grid already validated; guard again
    [_envNames addObject:name];
    [_dirtyEnvs addObject:name];
    [_removedEnvs removeObject:name];
    for (NSString *alias in _aliases) _values[Key(name, alias)] = @"";
    [self pushToGrid];
}

- (void)envGrid:(OS9EnvGrid *)g deleteEnv:(NSString *)env {
    if ([env isEqualToString:GlobalEnvName()]) return;   // reserved base
    NSInteger r = [OS9Dialog confirmWithTitle:StrDlgDeleteEnv
                                      message:[NSString stringWithFormat:StrFmtConfirmDeleteEnv, env]
                                      buttons:@[ StrCancel, StrDelete ]
                                defaultButton:1 cancelButton:0 icon:OS9AlertNone
                                       parent:_grid.window];
    if (r != 1) return;
    for (NSString *alias in _aliases) [_values removeObjectForKey:Key(env, alias)];
    [_envNames removeObject:env];
    [_dirtyEnvs removeObject:env];
    [_removedEnvs addObject:env];
    if (_sessionRepo && _sessionRepo->getActiveEnv() == std::string(env.UTF8String))
        _sessionRepo->setActiveEnv("");   // deleted the active env -> none selected
    [self pushToGrid];
}

- (void)envGrid:(OS9EnvGrid *)g addAliasNamed:(NSString *)name {
    name = [name uppercaseString];                       // variables always stored UPPER
    if (!name.length || [_aliases containsObject:name]) return;
    [_aliases addObject:name];
    for (NSString *env in _envNames) { _values[Key(env, name)] = @""; [_dirtyEnvs addObject:env]; }
    [self pushToGrid];
}

- (void)envGrid:(OS9EnvGrid *)g deleteAlias:(NSString *)alias {
    NSInteger r = [OS9Dialog confirmWithTitle:StrDlgDeleteAlias
                                      message:[NSString stringWithFormat:StrFmtConfirmDeleteAlias, alias]
                                      buttons:@[ StrCancel, StrDelete ]
                                defaultButton:1 cancelButton:0 icon:OS9AlertNone
                                       parent:_grid.window];
    if (r != 1) return;
    for (NSString *env in _envNames) { [_values removeObjectForKey:Key(env, alias)]; [_dirtyEnvs addObject:env]; }
    [_aliases removeObject:alias];
    [_secretAliases removeObject:alias];
    [self pushToGrid];
}

#pragma mark helpers

- (void)errorDialog:(NSString *)msg {
    [OS9Dialog confirmWithTitle:StrDlgInvalidTitle message:msg
                        buttons:@[ StrOK ] defaultButton:0 cancelButton:-1
                           icon:OS9AlertStop parent:_grid.window];
}

// SPEC §T3: renaming an alias does NOT auto-fix {{old}} in saved requests -> warn.
- (void)warnVarRename:(NSString *)oldAlias {
    [self showToast:[NSString stringWithFormat:StrFmtVarRenamed, oldAlias] kind:0];
}

- (void)showToast:(NSString *)msg kind:(NSInteger)kind {
    NSWindow *win = _grid.window;
    if (!win) return;
    NSView *cv = win.contentView;
    // L4: cap the toast stack — drop any existing toast before adding a new one (rapid alias renames must
    // not pile up dozens of overlapping subviews).
    for (NSView *sub in [cv.subviews copy])
        if ([sub isKindOfClass:[OS9Toast class]]) [sub removeFromSuperview];
    OS9Toast *t = [[OS9Toast alloc] initWithMessage:msg kind:kind];
    NSSize sz = [OS9Toast sizeForMessage:msg];
    t.frame = NSMakeRect((cv.bounds.size.width - sz.width) / 2, 31, sz.width, sz.height);   // below title bar
    t.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMaxYMargin;
    __weak OS9Toast *wt = t;
    t.onClose = ^{ [wt removeFromSuperview]; };
    [cv addSubview:t];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [wt removeFromSuperview];
    });
}

@end
