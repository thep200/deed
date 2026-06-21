// SciTextView — thin wrapper around ScintillaView (docs/RENDERING_AND_ASSETS.md §3.6).
// Hides the SCI_* API from the rest; used for both the request pane (editable) and response (read-only).
#import <Cocoa/Cocoa.h>

@interface SciTextView : NSView
@property(nonatomic, copy) NSString *string;          // get/set the entire text
@property(nonatomic) BOOL editable;                   // toggle editing (response = NO)
@property(nonatomic, copy) void (^onTextChanged)(void); // called when the USER edits (not when set by code)

- (instancetype)initEditable:(BOOL)editable;          // create + configure JSON + Platinum theme
- (void)setFontName:(NSString *)name size:(CGFloat)size;
- (BOOL)hasFocus;                                     // is the editor holding the caret?
// Free the text + undo buffers (LAZY_TREE §8.3): clear text then empty the undo buffer.
// Call when switching/closing a request so old content isn't kept in RAM.
- (void)clearContents;
// Safe teardown (CRASH_FIX_LIFECYCLE §2.2): resign input context + detach the Scintilla delegate
// (unsafe_unretained) before destruction -> don't send notifications to a dead object. Idempotent;
// also called automatically in dealloc.
- (void)teardown;
@end
