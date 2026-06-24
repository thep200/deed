// MainWindowController(Stress) — debug API for StressRunner (STRESS_TEST.md §5).
// Implemented INSIDE MainWindowController.mm under `#if DEED_DEBUG_TOOLS` (needs ivar access).
// NOT compiled into release.
#import "windows/MainWindowController.h"

#if DEED_DEBUG_TOOLS

@interface MainWindowController (Stress)

// Build a temp collection + open it + create a few requests (so the runner can run without the user opening a folder).
- (void)stressBootstrap;

// List of existing request relPaths (scans the tree). Empty if no collection is open.
- (NSArray<NSString *> *)stressRequestRels;
- (NSString *)stressOpenRequestId;          // id of the open request (empty if idle)
- (uint64_t)stressRamCacheBytes;

// Operations that EXERCISE the exact lifecycle paths that caused crashes (first-responder / Scintilla / window):
- (void)stressLoadRel:(NSString *)rel;       // load + put first responder on URL (real input context)
- (void)stressSwitchRandom:(uint32_t)r;      // switch to another request (teardown path §2.1)
- (void)stressTypeRandom:(uint32_t)r;        // focus URL/editor then insert text
- (void)stressToggleRandomFolder:(uint32_t)r;// expand/collapse folder (lazy scan)
- (void)stressEnterEnv;                      // open ENV config (field editor + table)
- (void)stressEnterSettings;                 // open Settings config (SciTextView)
- (void)stressExitConfig;                    // back (resign + teardown editor §2.1)
- (void)stressPickRandomEnv:(uint32_t)r;     // change active env
- (void)stressInjectResponse:(BOOL)large;    // inject a fake response (small / ~20MB) into view + cache
- (void)stressRenameAutoDismiss:(uint32_t)r; // present rename dialog then auto-dismiss (path §2.3)
- (void)stressGoIdle;                        // return to no-open-request state (baseline)

@end

#endif // DEED_DEBUG_TOOLS
