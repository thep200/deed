// JsonEditorBehavior — các hành vi soạn JSON cho Scintilla (docs/EDITOR_BEHAVIORS.md).
// Chỉ dùng cho ô SOẠN (editable); response read-only không cần.
// Gom logic auto-close / auto-indent / skip-over / brace-match vào một lớp,
// tách khỏi phần dựng UI. SciTextView route SCN_CHARADDED/SCN_UPDATEUI vào đây.
#import <Cocoa/Cocoa.h>

@class ScintillaView;

@interface JsonEditorBehavior : NSObject
- (instancetype)initWithScintillaView:(ScintillaView *)sci;

@property(nonatomic) BOOL autoClose;    // tự đóng cặp () [] {} "" + skip-over (mặc định YES)
@property(nonatomic) BOOL autoIndent;   // tự thụt dòng khi Enter (mặc định YES)
@property(nonatomic) BOOL braceMatch;   // tô sáng cặp ngoặc khớp (mặc định YES)

- (void)handleCharAdded:(unsigned)ch;   // gọi từ SCN_CHARADDED
- (void)updateBraceMatch;               // gọi từ SCN_UPDATEUI
- (void)applyHighlightStyles;           // màu brace-light/bad (gọi sau khi set theme)
@end
