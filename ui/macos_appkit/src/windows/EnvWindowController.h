#import <Cocoa/Cocoa.h>

#import "widgets/OS9EnvGrid.h"

namespace core { class Engine; }

// ENV matrix (row=alias, col=env; col 0 = base shown as "Local"). SPEC §T1–T5.
// Self-drawn via OS9EnvGrid (no NSTableView). Inline actions in the table (no
// bottom button strip). Embedded in the Config screen: vends an NSView.
@interface EnvWindowController : NSObject <OS9EnvGridDelegate>
- (instancetype)initWithEngine:(core::Engine *)engine;
- (NSView *)view;     // build (lazy) + return container to embed
- (void)reload;       // reload from store
- (void)save;         // write edited envs (atomic, plaintext)
- (void)layout;       // re-layout subviews to view's current bounds
@end
