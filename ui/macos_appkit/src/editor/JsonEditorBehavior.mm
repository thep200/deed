#import "editor/JsonEditorBehavior.h"

#import <ScintillaView.h>
#include <Scintilla.h>
#include <SciLexer.h>

@implementation JsonEditorBehavior {
    ScintillaView *_sci;
}

- (instancetype)initWithScintillaView:(ScintillaView *)sci {
    if ((self = [super init])) {
        _sci = sci;
        _autoClose = YES;
        _autoIndent = YES;
        _braceMatch = YES;
        // Thụt bằng SPACE (JSON pretty), 2 khoảng / cấp.
        [self msg:SCI_SETUSETABS w:0 l:0];
        [self msg:SCI_SETINDENT w:2 l:0];
    }
    return self;
}

- (sptr_t)msg:(unsigned)m w:(uptr_t)w l:(sptr_t)l { return [_sci message:m wParam:w lParam:l]; }
- (sptr_t)pos { return [self msg:SCI_GETCURRENTPOS w:0 l:0]; }
- (char)charAt:(sptr_t)p { return (p < 0) ? 0 : (char)[self msg:SCI_GETCHARAT w:(uptr_t)p l:0]; }

#pragma mark - (f) brace-match

- (void)applyHighlightStyles {
    // Cặp khớp: đậm + nền vàng nhạt; cặp hỏng: chữ đỏ.
    [self msg:SCI_STYLESETBOLD w:STYLE_BRACELIGHT l:1];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:STYLE_BRACELIGHT
                     value:[NSColor colorWithCalibratedRed:0.0 green:0.0 blue:0.55 alpha:1]];
    [_sci setColorProperty:SCI_STYLESETBACK parameter:STYLE_BRACELIGHT
                     value:[NSColor colorWithCalibratedRed:1.0 green:0.92 blue:0.55 alpha:1]];
    [self msg:SCI_STYLESETBOLD w:STYLE_BRACEBAD l:1];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:STYLE_BRACEBAD
                     value:[NSColor colorWithCalibratedRed:0.8 green:0.0 blue:0.0 alpha:1]];
}

static BOOL IsBrace(char c) {
    return c=='('||c==')'||c=='['||c==']'||c=='{'||c=='}';
}

- (void)updateBraceMatch {
    if (!_braceMatch) return;
    sptr_t pos = [self pos];
    // Xét ngoặc ngay TẠI caret rồi đến ngay TRƯỚC caret (như đa số editor).
    sptr_t cands[2] = { pos, pos - 1 };
    for (int i = 0; i < 2; i++) {
        sptr_t p = cands[i];
        if (p < 0) continue;
        if (!IsBrace([self charAt:p])) continue;
        sptr_t m = [self msg:SCI_BRACEMATCH w:(uptr_t)p l:0];
        if (m != -1) { [self msg:SCI_BRACEHIGHLIGHT w:(uptr_t)p l:m]; return; }
        [self msg:SCI_BRACEBADLIGHT w:(uptr_t)p l:0]; return;   // có ngoặc nhưng không khớp
    }
    [self msg:SCI_BRACEHIGHLIGHT w:(uptr_t)-1 l:(sptr_t)-1];     // không có ngoặc -> xoá sáng
}

#pragma mark - (b)+(c) char added: auto-close / skip-over / auto-indent

- (void)handleCharAdded:(unsigned)ch {
    if (ch == '\n' || ch == '\r') { if (_autoIndent) [self autoIndentNewLine]; return; }
    if (!_autoClose) return;

    // (c) skip-over: gõ closer mà bên phải đã sẵn closer đó -> nhảy qua, không để dư.
    if (ch==')' || ch==']' || ch=='}' || ch=='"') {
        if ([self skipOverCloser:(char)ch]) return;
    }

    const char *close = NULL;
    switch (ch) {
        case '(': close = ")";  break;
        case '[': close = "]";  break;
        case '{': close = "}";  break;
        case '"': if (![self shouldCloseQuote]) return; close = "\""; break;
        default: return;
    }
    sptr_t pos = [self pos];
    [self msg:SCI_BEGINUNDOACTION w:0 l:0];     // Ctrl+Z xoá cả cặp một lần
    [self msg:SCI_INSERTTEXT w:(uptr_t)pos l:(sptr_t)close];
    [self msg:SCI_SETSEL w:(uptr_t)pos l:pos];  // caret nằm giữa cặp
    [self msg:SCI_ENDUNDOACTION w:0 l:0];
}

// Ký tự vừa gõ (closer) đã được chèn ở pos-1; nếu ngay sau caret cũng là closer đó
// (vd closer auto-close trước đó) -> xoá cái vừa gõ, đẩy caret qua closer có sẵn.
- (BOOL)skipOverCloser:(char)c {
    sptr_t pos = [self pos];
    if ([self charAt:pos] != c) return NO;
    [self msg:SCI_BEGINUNDOACTION w:0 l:0];
    [self msg:SCI_DELETERANGE w:(uptr_t)(pos - 1) l:1];  // bỏ closer vừa gõ
    [self msg:SCI_SETSEL w:(uptr_t)pos l:pos];           // caret sau closer có sẵn
    [self msg:SCI_ENDUNDOACTION w:0 l:0];
    return YES;
}

// Heuristic nhẹ (thay cho string-awareness đầy đủ): chỉ auto-close " khi bên phải
// là ranh giới hợp lý (rỗng/space/, } ] :) -> tránh đóng " giữa từ đang gõ dở.
- (BOOL)shouldCloseQuote {
    char next = [self charAt:[self pos]];
    return next==0 || next==' ' || next=='\t' || next=='\n' || next=='\r' ||
           next==',' || next=='}' || next==']' || next==':';
}

#pragma mark - (b) auto-indent

// Ký tự cuối (bỏ khoảng trắng) của 1 dòng — để biết dòng trước kết thúc bằng { hay [.
- (char)lastNonSpaceOfLine:(sptr_t)line {
    sptr_t start = [self msg:SCI_POSITIONFROMLINE w:(uptr_t)line l:0];
    sptr_t end   = [self msg:SCI_GETLINEENDPOSITION w:(uptr_t)line l:0];
    for (sptr_t p = end - 1; p >= start; p--) {
        char c = [self charAt:p];
        if (c==' ' || c=='\t') continue;
        return c;
    }
    return 0;
}

- (void)autoIndentNewLine {
    sptr_t pos = [self pos];
    sptr_t line = [self msg:SCI_LINEFROMPOSITION w:(uptr_t)pos l:0];
    if (line == 0) return;
    sptr_t unit = [self msg:SCI_GETINDENT w:0 l:0]; if (unit <= 0) unit = 2;
    sptr_t prevIndent = [self msg:SCI_GETLINEINDENTATION w:(uptr_t)(line - 1) l:0];

    char opener = [self lastNonSpaceOfLine:line - 1];   // dòng vừa xuống dòng từ đó
    char closer = [self charAt:pos];                    // ký tự ngay sau caret
    BOOL openPair = (opener=='{' || opener=='[');
    BOOL matchedPair = (opener=='{' && closer=='}') || (opener=='[' && closer==']');

    [self msg:SCI_BEGINUNDOACTION w:0 l:0];
    if (matchedPair) {
        // Enter giữa {|} -> tạo dòng rỗng thụt sâu, đẩy } xuống dòng riêng cùng cấp mở.
        [self msg:SCI_SETLINEINDENTATION w:(uptr_t)line l:prevIndent + unit];
        sptr_t ip = [self msg:SCI_GETLINEINDENTPOSITION w:(uptr_t)line l:0];
        [self msg:SCI_INSERTTEXT w:(uptr_t)ip l:(sptr_t)"\n"];
        [self msg:SCI_SETLINEINDENTATION w:(uptr_t)(line + 1) l:prevIndent];
        [self msg:SCI_SETSEL w:(uptr_t)ip l:ip];
    } else {
        sptr_t indent = prevIndent + (openPair ? unit : 0);
        [self msg:SCI_SETLINEINDENTATION w:(uptr_t)line l:indent];
        sptr_t ip = [self msg:SCI_GETLINEINDENTPOSITION w:(uptr_t)line l:0];
        [self msg:SCI_SETSEL w:(uptr_t)ip l:ip];
    }
    [self msg:SCI_ENDUNDOACTION w:0 l:0];
}

@end
