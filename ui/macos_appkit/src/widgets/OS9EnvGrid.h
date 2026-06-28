// OS9EnvGrid — self-drawn Platinum-style Environment table (SPEC §T1/§T2/§T3).
// Matrix: row = alias, col = env (col 0 = base "Local"). Self-draws header/zebra/grid/
// selection; only the edit field uses a native NSTextField overlay when editing text.
// Inline actions: × delete env in header (except Local col), + add env, "+ alias" row,
// × delete alias at row start. Does not use NSTableView.
#import <Cocoa/Cocoa.h>

@class OS9EnvGrid;

@protocol OS9EnvGridDelegate <NSObject>
// Cell data (controller holds the view-model).
- (NSString *)envGrid:(OS9EnvGrid *)g valueForAlias:(NSString *)alias env:(NSString *)env;
// Commit edit.
- (void)envGrid:(OS9EnvGrid *)g setValue:(NSString *)val forAlias:(NSString *)alias env:(NSString *)env;
- (void)envGrid:(OS9EnvGrid *)g renameAlias:(NSString *)oldAlias to:(NSString *)newAlias;
- (void)envGrid:(OS9EnvGrid *)g renameEnv:(NSString *)oldEnv to:(NSString *)newEnv;
// Per-alias "secret" flag (last column toggle). Read for drawing; write on toggle.
- (BOOL)envGrid:(OS9EnvGrid *)g isSecretForAlias:(NSString *)alias;
- (void)envGrid:(OS9EnvGrid *)g setSecret:(BOOL)secret forAlias:(NSString *)alias;
// Actions (name already validated by grid: non-empty + no duplicates).
- (void)envGrid:(OS9EnvGrid *)g addEnvNamed:(NSString *)name;
- (void)envGrid:(OS9EnvGrid *)g deleteEnv:(NSString *)env;
- (void)envGrid:(OS9EnvGrid *)g addAliasNamed:(NSString *)name;
- (void)envGrid:(OS9EnvGrid *)g deleteAlias:(NSString *)alias;
@end

@interface OS9EnvGrid : NSView

@property(nonatomic, weak) id<OS9EnvGridDelegate> delegate;

// Column order (index 0 = base env). Names here are the internal KEY (e.g. "Global").
@property(nonatomic, copy) NSArray<NSString *> *envNames;
// Row order (alias names).
@property(nonatomic, copy) NSArray<NSString *> *aliases;
// Display label for the base column (e.g. "Local"); internal key is still envNames[0].
@property(nonatomic, copy) NSString *baseDisplayName;
// Selected row (-1 = none).
@property(nonatomic) NSInteger selectedRow;

// Redraw from current data.
- (void)reloadData;
// Commit/cancel the cell being edited (call before leaving the screen / save). No-op for dialog-style edit.
- (void)commitEditing;

@end
