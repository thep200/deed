#import "editor/SciTextView.h"
#import "app/OS9Lifecycle.h"
#import "theme/OS9Theme.h"
#import "widgets/OS9Scroller.h"
#import "editor/JsonEditorBehavior.h"

#import <ScintillaView.h>
#include <Scintilla.h>
#include <SciLexer.h>
#include "deed_lexers.h"

#include <string>
#include "core/codec/comment.hpp"

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
- (sptr_t)q:(unsigned int)m w:(uptr_t)w l:(sptr_t)l { return [_sci message:m wParam:w lParam:l]; }

// §2.2: deactivate input context của Scintilla rồi NGẮT delegate (unsafe_unretained) TRƯỚC khi
// view bị huỷ. Nếu không, ScintillaView có thể bắn notification vào self đã giải phóng, hoặc
// updateWindows kích hoạt lại input context của content view đã chết -> use-after-free.
- (void)teardown {
    OS9SafeEndEditing(self.window, self);
    _sci.delegate = nil;   // unsafe_unretained -> phải nil thủ công, ARC không lo hộ
    _behavior = nil;       // thả hành vi JSON (giữ con trỏ ScintillaView)
}

- (void)dealloc { [self teardown]; }

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

#pragma mark Comment toggle (Ctrl+/ / Cmd+/) — SPEC §T7

// performKeyEquivalent được gọi cho MỌI keyDown trước khi Scintilla xử lý -> chặn được
// "/" + Ctrl/Cmd. Chỉ tác động nếu view này đang focus + editable + có commentMode.
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if (_editable && _commentMode.length && [self hasFocus]) {
        NSString *ch = event.charactersIgnoringModifiers;
        NSEventModifierFlags m = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        BOOL hasCmdOrCtrl = (m & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;
        BOOL noShift = (m & NSEventModifierFlagShift) == 0;   // Shift+/ = '?'
        // KHÔNG yêu cầu "chỉ Cmd/Ctrl" (bàn phím có thể kèm cờ Function/NumericPad cho '/').
        if ([ch isEqualToString:@"/"] && hasCmdOrCtrl && noShift) {
            [self toggleCommentSelection];
            return YES;
        }
    }
    return [super performKeyEquivalent:event];
}

// Dòng L có bắt đầu (sau indent) bằng prefix không? -1 = dòng trắng, 0 = không, 1 = có.
- (int)line:(sptr_t)L startsWith:(const std::string &)prefix {
    sptr_t ip = [self q:SCI_GETLINEINDENTPOSITION w:(uptr_t)L l:0];
    sptr_t le = [self q:SCI_GETLINEENDPOSITION w:(uptr_t)L l:0];
    if (ip >= le) return -1; // trắng
    for (size_t k = 0; k < prefix.size(); k++) {
        if (ip + (sptr_t)k >= le) return 0;
        if ((char)[self q:SCI_GETCHARAT w:(uptr_t)(ip + k) l:0] != prefix[k]) return 0;
    }
    return 1;
}

- (void)toggleLineComment:(const std::string &)prefix {
    sptr_t selStart = [self q:SCI_GETSELECTIONSTART w:0 l:0];
    sptr_t selEnd = [self q:SCI_GETSELECTIONEND w:0 l:0];
    sptr_t lineFrom = [self q:SCI_LINEFROMPOSITION w:(uptr_t)selStart l:0];
    sptr_t lineTo = [self q:SCI_LINEFROMPOSITION w:(uptr_t)selEnd l:0];
    // selection kết thúc đúng đầu dòng cuối -> không tính dòng đó.
    if (lineTo > lineFrom && selEnd == [self q:SCI_POSITIONFROMLINE w:(uptr_t)lineTo l:0]) lineTo--;

    // Quyết định comment/uncomment: TẤT CẢ dòng không-trắng đã có prefix -> uncomment.
    BOOL allCommented = YES; BOOL anyNonBlank = NO;
    for (sptr_t L = lineFrom; L <= lineTo; L++) {
        int r = [self line:L startsWith:prefix];
        if (r == -1) continue;
        anyNonBlank = YES;
        if (r == 0) { allCommented = NO; break; }
    }
    BOOL uncomment = (anyNonBlank && allCommented);
    std::string ins = prefix + " ";

    [self msg:SCI_BEGINUNDOACTION w:0 l:0];
    for (sptr_t L = lineFrom; L <= lineTo; L++) {
        sptr_t ip = [self q:SCI_GETLINEINDENTPOSITION w:(uptr_t)L l:0];
        sptr_t le = [self q:SCI_GETLINEENDPOSITION w:(uptr_t)L l:0];
        if (ip >= le) continue; // bỏ qua dòng trắng
        if (uncomment) {
            sptr_t del = (sptr_t)prefix.size();
            // xoá thêm 1 space sau marker nếu có.
            if (ip + del < le && (char)[self q:SCI_GETCHARAT w:(uptr_t)(ip + del) l:0] == ' ') del++;
            [self msg:SCI_DELETERANGE w:(uptr_t)ip l:del];
        } else {
            [self msg:SCI_INSERTTEXT w:(uptr_t)ip l:(sptr_t)ins.c_str()];
        }
    }
    [self msg:SCI_ENDUNDOACTION w:0 l:0];

    // chọn lại trọn vùng dòng đã đổi.
    sptr_t a = [self q:SCI_POSITIONFROMLINE w:(uptr_t)lineFrom l:0];
    sptr_t b = [self q:SCI_GETLINEENDPOSITION w:(uptr_t)lineTo l:0];
    [self msg:SCI_SETSEL w:(uptr_t)a l:b];
}

- (void)toggleBlockComment:(const core::codec::CommentMarker &)mk {
    sptr_t s = [self q:SCI_GETSELECTIONSTART w:0 l:0];
    sptr_t e = [self q:SCI_GETSELECTIONEND w:0 l:0];
    if (s == e) { // không chọn -> trọn dòng hiện tại
        sptr_t L = [self q:SCI_LINEFROMPOSITION w:(uptr_t)s l:0];
        s = [self q:SCI_GETLINEINDENTPOSITION w:(uptr_t)L l:0];
        e = [self q:SCI_GETLINEENDPOSITION w:(uptr_t)L l:0];
    }
    // Đã là block? kiểm ký tự đầu == blockOpen.
    auto charsAt = [&](sptr_t pos, size_t n) {
        std::string out;
        for (size_t k = 0; k < n; k++) out.push_back((char)[self q:SCI_GETCHARAT w:(uptr_t)(pos + k) l:0]);
        return out;
    };
    BOOL wrapped = (charsAt(s, mk.blockOpen.size()) == mk.blockOpen) &&
                   (e - (sptr_t)mk.blockClose.size() >= s) &&
                   (charsAt(e - (sptr_t)mk.blockClose.size(), mk.blockClose.size()) == mk.blockClose);
    [self msg:SCI_BEGINUNDOACTION w:0 l:0];
    if (wrapped) {
        [self msg:SCI_DELETERANGE w:(uptr_t)(e - (sptr_t)mk.blockClose.size()) l:(sptr_t)mk.blockClose.size()];
        [self msg:SCI_DELETERANGE w:(uptr_t)s l:(sptr_t)mk.blockOpen.size()];
    } else {
        std::string close = " " + mk.blockClose;
        std::string open = mk.blockOpen + " ";
        [self msg:SCI_INSERTTEXT w:(uptr_t)e l:(sptr_t)close.c_str()];
        [self msg:SCI_INSERTTEXT w:(uptr_t)s l:(sptr_t)open.c_str()];
    }
    [self msg:SCI_ENDUNDOACTION w:0 l:0];
}

- (void)toggleCommentSelection {
    if (!_editable || !_commentMode.length) return;
    core::codec::CommentMarker mk = core::codec::commentMarkerFor(_commentMode.UTF8String);
    if (mk.hasBlock()) [self toggleBlockComment:mk];
    else if (mk.hasLine()) [self toggleLineComment:mk.linePrefix];
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
