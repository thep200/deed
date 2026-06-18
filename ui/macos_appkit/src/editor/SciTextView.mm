#import "editor/SciTextView.h"
#import "theme/OS9Theme.h"
#import "widgets/OS9Scroller.h"
#import "editor/JsonEditorBehavior.h"

#import <ScintillaView.h>
#include <Scintilla.h>
#include <SciLexer.h>
#include "deed_lexers.h"

@interface SciTextView () <ScintillaNotificationProtocol>
@end

@implementation SciTextView {
    ScintillaView *_sci;
    BOOL _programmatic; // đang set text bằng code -> KHÔNG bắn onTextChanged
    JsonEditorBehavior *_behavior; // hành vi soạn JSON; nil cho ô read-only
}

- (instancetype)initEditable:(BOOL)editable {
    if ((self = [super initWithFrame:NSZeroRect])) {
        _editable = editable;
        _sci = [[ScintillaView alloc] initWithFrame:self.bounds];
        _sci.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _sci.delegate = self;
        [self addSubview:_sci];
        [self configure];
    }
    return self;
}

- (void)msg:(unsigned int)m w:(uptr_t)w l:(sptr_t)l { [_sci message:m wParam:w lParam:l]; }

- (void)configure {
    // Lexer JSON + lề số dòng + idle styling (chi phí ~ vùng nhìn thấy).
    [self msg:SCI_SETILEXER w:0 l:(sptr_t)DeedCreateJSONLexer()];
    [self msg:SCI_SETWRAPMODE w:SC_WRAP_NONE l:0];
    [self msg:SCI_SETIDLESTYLING w:SC_IDLESTYLING_ALL l:0];

    // lề 0 = số dòng. KHÔNG dùng lề fold (bỏ các nốt tròn ở đầu mỗi dòng).
    [self msg:SCI_SETMARGINTYPEN w:0 l:SC_MARGIN_NUMBER];
    [self msg:SCI_SETMARGINWIDTHN w:0 l:34];
    [self msg:SCI_SETMARGINWIDTHN w:1 l:0]; // lề symbol/fold = 0 -> không hiện marker

    // Thanh cuộn ngang: dòng dài (không wrap) -> hiện scrollbar kéo trái/phải.
    // Tracking để Scintilla tự nới bề rộng cuộn theo dòng dài nhất.
    [self msg:SCI_SETHSCROLLBAR w:1 l:0];
    [self msg:SCI_SETSCROLLWIDTHTRACKING w:1 l:0];
    [self msg:SCI_SETSCROLLWIDTH w:1 l:0]; // mốc tối thiểu; tracking sẽ tự tăng

    [self applyPlatinumTheme];
    [_sci setEditable:_editable];
    [self msg:SCI_SETREADONLY w:(_editable ? 0 : 1) l:0];
    // Response (read-only): KHÔNG tích undo -> response lớn không phình undo buffer (§8.3).
    if (!_editable) [self msg:SCI_SETUNDOCOLLECTION w:0 l:0];

    // Scrollbar RETRO: dùng OS9Scroller (như cây thư mục) thay scroller hệ thống.
    // Overlay + autohide -> ẩn khi không cuộn, chỉ hiện lúc có scroll event.
    NSScrollView *sv = [(NSView *)[_sci content] enclosingScrollView];
    if (sv) {
        sv.scrollerStyle = NSScrollerStyleOverlay;
        sv.autohidesScrollers = YES;
        sv.scrollerKnobStyle = NSScrollerKnobStyleDefault;
        sv.hasVerticalScroller = YES;
        sv.hasHorizontalScroller = YES;
        sv.verticalScroller = [[OS9Scroller alloc] initWithFrame:NSMakeRect(0, 0, 16, 100)];
        sv.horizontalScroller = [[OS9Scroller alloc] initWithFrame:NSMakeRect(0, 0, 100, 16)];
    }

    // Hành vi soạn JSON (auto-close / auto-indent / brace-match): chỉ ô SOẠN.
    if (_editable) {
        _behavior = [[JsonEditorBehavior alloc] initWithScintillaView:_sci];
        [_behavior applyHighlightStyles];
    }
}

- (void)applyPlatinumTheme {
    NSString *font = @"Monaco";
    [self msg:SCI_STYLESETFONT w:STYLE_DEFAULT l:(sptr_t)font.UTF8String];
    [self msg:SCI_STYLESETSIZE w:STYLE_DEFAULT l:11];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:STYLE_DEFAULT value:[NSColor blackColor]];
    [_sci setColorProperty:SCI_STYLESETBACK parameter:STYLE_DEFAULT value:[NSColor whiteColor]];
    [self msg:SCI_STYLECLEARALL w:0 l:0]; // áp default cho mọi style

    // số dòng: nền platinum, chữ xám
    [_sci setColorProperty:SCI_STYLESETBACK parameter:STYLE_LINENUMBER value:[OS9Theme buttonFace]];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:STYLE_LINENUMBER value:[NSColor colorWithCalibratedWhite:0.4 alpha:1]];

    // màu cú pháp JSON
    NSColor *green = [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1];
    NSColor *blue = [NSColor colorWithCalibratedRed:0.1 green:0.2 blue:0.8 alpha:1];
    NSColor *purple = [NSColor colorWithCalibratedRed:0.45 green:0.1 blue:0.5 alpha:1];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_STRING value:green];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_STRINGEOL value:green];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_NUMBER value:blue];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_PROPERTYNAME value:purple];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_KEYWORD value:blue];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_OPERATOR value:[NSColor blackColor]];

    // caret + selection
    [_sci setColorProperty:SCI_SETCARETFORE parameter:0 value:[NSColor blackColor]];
    [_sci setColorProperty:SCI_SETSELBACK parameter:1 value:[NSColor colorWithCalibratedRed:0.78 green:0.82 blue:0.95 alpha:1]];
}

- (void)setFontName:(NSString *)name size:(CGFloat)size {
    if (name.length) [self msg:SCI_STYLESETFONT w:STYLE_DEFAULT l:(sptr_t)name.UTF8String];
    if (size > 0) [self msg:SCI_STYLESETSIZE w:STYLE_DEFAULT l:(sptr_t)size];
    [self msg:SCI_STYLECLEARALL w:0 l:0];
    [self applyPlatinumTheme];
    [_behavior applyHighlightStyles];   // STYLECLEARALL xoá -> set lại màu brace
}

#pragma mark text get/set

- (NSString *)string { return [_sci string] ?: @""; }

- (void)setString:(NSString *)string {
    _programmatic = YES;
    [self msg:SCI_SETREADONLY w:0 l:0];            // mở khoá để set được
    [_sci setString:string ?: @""];
    [self msg:SCI_SETREADONLY w:(_editable ? 0 : 1) l:0];
    [self msg:SCI_EMPTYUNDOBUFFER w:0 l:0];        // §8.3: bỏ lịch sử undo -> không giữ bản text cũ
    [self msg:SCI_SETSCROLLWIDTH w:1 l:0];         // reset scroll width
    [self msg:SCI_GOTOPOS w:0 l:0];
    _programmatic = NO;
}

- (void)clearContents {
    _programmatic = YES;
    [self msg:SCI_SETREADONLY w:0 l:0];
    [self msg:SCI_CLEARALL w:0 l:0];               // xoá toàn bộ text
    [self msg:SCI_SETREADONLY w:(_editable ? 0 : 1) l:0];
    [self msg:SCI_EMPTYUNDOBUFFER w:0 l:0];        // xoá undo -> giải phóng bản sao nội dung cũ
    [self msg:SCI_SETSCROLLWIDTH w:1 l:0];
    _programmatic = NO;
}

- (void)setEditable:(BOOL)editable {
    _editable = editable;
    [_sci setEditable:editable];
    [self msg:SCI_SETREADONLY w:(editable ? 0 : 1) l:0];
}

- (BOOL)hasFocus {
    NSResponder *r = self.window.firstResponder;
    return ([r isKindOfClass:[NSView class]] && [(NSView *)r isDescendantOf:_sci]);
}

// Click vào vùng TRỐNG dưới text rơi vào NSClipView (document view chỉ cao bằng
// số dòng) -> không tới SCIContentView -> không giành được focus. Chuyển hit về
// content view để click đâu trong editor cũng đặt được con trỏ + nhận bàn phím.
- (NSView *)hitTest:(NSPoint)point {
    NSView *v = [super hitTest:point];
    if ([v isKindOfClass:[NSClipView class]]) {
        NSView *content = (NSView *)[_sci content];
        if (content) return content;
    }
    return v;
}

#pragma mark Scintilla notifications

- (void)notification:(SCNotification *)n {
    switch (n->nmhdr.code) {
        case SCN_CHARADDED:                          // (b)(c) auto-close/indent/skip-over
            if (!_programmatic) [_behavior handleCharAdded:(unsigned)n->ch];
            break;
        case SCN_UPDATEUI:                           // (f) brace-match (no-op nếu _behavior nil)
            [_behavior updateBraceMatch];
            break;
        case SCN_MODIFIED:
            if (!_programmatic &&
                (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
                if (_onTextChanged) _onTextChanged();
            }
            break;
        default: break;
    }
}

@end
