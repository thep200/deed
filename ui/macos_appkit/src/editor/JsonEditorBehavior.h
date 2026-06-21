// JsonEditorBehavior — JSON editing behaviors for Scintilla (docs/EDITOR_BEHAVIORS.md).
// Only for EDITABLE fields; read-only responses don't need it.
// Gathers auto-close / auto-indent / skip-over / brace-match logic into one class,
// separate from UI construction. SciTextView routes SCN_CHARADDED/SCN_UPDATEUI here.
#import <Cocoa/Cocoa.h>

@class ScintillaView;

@interface JsonEditorBehavior : NSObject
- (instancetype)initWithScintillaView:(ScintillaView *)sci;

@property(nonatomic) BOOL autoClose;    // auto-close pairs () [] {} "" + skip-over (default YES)
@property(nonatomic) BOOL autoIndent;   // auto-indent on Enter (default YES)
@property(nonatomic) BOOL braceMatch;   // highlight matching brace pair (default YES)

- (void)handleCharAdded:(unsigned)ch;   // called from SCN_CHARADDED
- (void)updateBraceMatch;               // called from SCN_UPDATEUI
- (void)applyHighlightStyles;           // brace-light/bad colors (call after setting theme)
@end
