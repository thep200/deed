// OS9EnvGrid — bảng Environment tự vẽ kiểu Platinum (SPEC §T1/§T2/§T3).
// Ma trận: hàng = alias, cột = env (cột 0 = base "Local"). Tự vẽ header/zebra/lưới/
// selection; chỉ ô nhập khi sửa text mới dùng NSTextField native overlay.
// Action inline: × xoá env ở header (trừ cột Local), + thêm env, hàng "+ alias",
// × xoá alias ở đầu hàng. Không dùng NSTableView.
#import <Cocoa/Cocoa.h>

@class OS9EnvGrid;

@protocol OS9EnvGridDelegate <NSObject>
// Dữ liệu cell (controller giữ view-model).
- (NSString *)envGrid:(OS9EnvGrid *)g valueForAlias:(NSString *)alias env:(NSString *)env;
// Commit sửa.
- (void)envGrid:(OS9EnvGrid *)g setValue:(NSString *)val forAlias:(NSString *)alias env:(NSString *)env;
- (void)envGrid:(OS9EnvGrid *)g renameAlias:(NSString *)oldAlias to:(NSString *)newAlias;
- (void)envGrid:(OS9EnvGrid *)g renameEnv:(NSString *)oldEnv to:(NSString *)newEnv;
// Action (tên đã được validate ở grid: non-empty + không trùng).
- (void)envGrid:(OS9EnvGrid *)g addEnvNamed:(NSString *)name;
- (void)envGrid:(OS9EnvGrid *)g deleteEnv:(NSString *)env;
- (void)envGrid:(OS9EnvGrid *)g addAliasNamed:(NSString *)name;
- (void)envGrid:(OS9EnvGrid *)g deleteAlias:(NSString *)alias;
@end

@interface OS9EnvGrid : NSView

@property(nonatomic, weak) id<OS9EnvGridDelegate> delegate;

// Thứ tự cột (index 0 = env base). Tên ở đây là KEY nội bộ (vd "Global").
@property(nonatomic, copy) NSArray<NSString *> *envNames;
// Thứ tự hàng (tên alias).
@property(nonatomic, copy) NSArray<NSString *> *aliases;
// Nhãn hiển thị cho cột base (vd "Local"); key nội bộ vẫn là envNames[0].
@property(nonatomic, copy) NSString *baseDisplayName;
// Hàng đang chọn (-1 = không).
@property(nonatomic) NSInteger selectedRow;

// Vẽ lại từ dữ liệu hiện tại.
- (void)reloadData;
// Commit/huỷ ô đang sửa (gọi trước khi rời màn / save). No-op với edit dạng dialog.
- (void)commitEditing;

@end
