// Deactivate NSTextInputContext BEFORE clearing/destroying a focused text view (or closing its window):
// otherwise the next -updateWindows re-activates the freed context -> EXC_BAD_ACCESS (worse under Vietnamese IME).
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
