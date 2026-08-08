// Auto-close / auto-indent / brace-match for EDITABLE Scintilla panes only; SciTextView routes SCN_CHARADDED/SCN_UPDATEUI here.
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
