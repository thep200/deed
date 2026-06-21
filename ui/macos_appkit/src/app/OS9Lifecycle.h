// OS9Lifecycle — first-responder / input-context teardown contract (docs/CRASH_FIX_LIFECYCLE.md §2).
//
// Root crash: a text view/field still acting as first responder gets its content cleared/destroyed,
// or the window holding it is closed, WITHOUT first deactivating NSTextInputContext -> the next
// event loop's -[NSApplication updateWindows] re-activates the freed input context -> EXC_BAD_ACCESS
// (worse with Vietnamese IME input since the IMK/TSM path lingers longer).
//
// Invariant: BEFORE destroying/changing/clearing a text view/field, or closing a window holding a
// text field -> call OS9SafeEndEditing while the object is still ALIVE to commit + release the input context cleanly.
#import <Cocoa/Cocoa.h>

// Call BEFORE clearing/replacing a text view, or before closing a window with a text field.
// dyingViewOrNil == nil  -> resign first responder unconditionally (closing window / removing many views).
// dyingViewOrNil != nil  -> resign only if the first responder IS that view (or a descendant of it).
static inline void OS9SafeEndEditing(NSWindow *w, NSView *dyingViewOrNil) {
    if (!w) return;
    [w endEditingFor:nil];                 // commit + release field editor (NSTextField)
    id fr = w.firstResponder;
    BOOL frIsDying = (dyingViewOrNil && (fr == dyingViewOrNil ||
                       ([fr isKindOfClass:NSView.class] && [(NSView *)fr isDescendantOf:dyingViewOrNil])));
    if (dyingViewOrNil == nil || frIsDying) {
        [w makeFirstResponder:nil];        // deactivate the dying view's NSTextInputContext
    }
}
