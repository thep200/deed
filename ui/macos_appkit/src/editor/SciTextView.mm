#import "editor/SciTextView.h"
#import "app/OS9Lifecycle.h"
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
    BOOL _programmatic; // setting text by code -> do NOT fire onTextChanged
    JsonEditorBehavior *_behavior; // JSON editing behavior; nil for read-only fields
    NSString *_fontFace;  // Scintilla font family (default = OS9Theme configured font, fallback Monaco)
    CGFloat _fontPt;      // Scintilla font size
    BOOL _streaming;      // streaming-write mode
    BOOL _followTail;     // auto-scroll to the end as chunks arrive (paused if the user scrolls up)
    SciLanguage _language; // active lexer/style set (default Json — the historical behavior)
}

- (instancetype)initEditable:(BOOL)editable {
    if ((self = [super initWithFrame:NSZeroRect])) {
        _editable = editable;
        // Clip the Scintilla content to our bounds. The parent OS9SerratedInset draws its border FIRST,
        // then subviews paint on top; without clipping, Scintilla's line-number gutter background bleeds
        // over the inset's bottom border when the document is scrolled. Clipping keeps it inside.
        self.clipsToBounds = YES;
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

// Deactivate Scintilla's input context then DETACH the delegate (unsafe_unretained) BEFORE
// the view is destroyed. Otherwise ScintillaView may fire a notification into a freed self, or
// updateWindows may reactivate the dead content view's input context -> use-after-free.
- (void)teardown {
    OS9SafeEndEditing(self.window, self);
    _sci.delegate = nil;   // unsafe_unretained -> must nil manually, ARC won't handle it
    _behavior = nil;       // release the JSON behavior (holds a pointer to ScintillaView)
}

- (void)dealloc { [self teardown]; }

- (void)configure {
    // Default lexer (JSON) + line-number margin + idle styling (cost ~ visible area).
    _language = SciLanguageJson;
    [self msg:SCI_SETILEXER w:0 l:(sptr_t)DeedCreateJSONLexer()];
    [self msg:SCI_SETWRAPMODE w:SC_WRAP_NONE l:0];
    [self msg:SCI_SETIDLESTYLING w:SC_IDLESTYLING_ALL l:0];

    // margin 0 = line numbers. Do NOT use a fold margin (drops the little dots at each line start).
    [self msg:SCI_SETMARGINTYPEN w:0 l:SC_MARGIN_NUMBER];
    [self msg:SCI_SETMARGINWIDTHN w:0 l:34];
    [self msg:SCI_SETMARGINWIDTHN w:1 l:0]; // symbol/fold margin = 0 -> no markers shown

    // Horizontal scroll bar: long (unwrapped) lines -> show a left/right scrollbar.
    // Tracking so Scintilla widens the scroll area to the longest line on its own.
    [self msg:SCI_SETHSCROLLBAR w:1 l:0];
    [self msg:SCI_SETSCROLLWIDTHTRACKING w:1 l:0];
    [self msg:SCI_SETSCROLLWIDTH w:1 l:0]; // minimum baseline; tracking grows it

    [self applyPlatinumTheme];
    [_sci setEditable:_editable];
    [self msg:SCI_SETREADONLY w:(_editable ? 0 : 1) l:0];
    // Response (read-only): do NOT collect undo -> large responses don't bloat the undo buffer.
    if (!_editable) [self msg:SCI_SETUNDOCOLLECTION w:0 l:0];

    // RETRO scrollbar: use OS9Scroller (like the folder tree) instead of the system scroller.
    // Overlay + autohide -> hidden when not scrolling, shown only on a scroll event.
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

    // JSON editing behavior (auto-close / auto-indent / brace-match): EDITABLE fields only.
    if (_editable) {
        _behavior = [[JsonEditorBehavior alloc] initWithScintillaView:_sci];
        [_behavior applyHighlightStyles];
    }
}

- (void)applyPlatinumTheme {
    // Font taken from ivar (default = OS9Theme configured font, fallback Monaco/11) -> Scintilla
    // uses the SAME font as the rest of the app. Do NOT hardcode "Monaco" (would override config).
    if (!_fontFace) {
        _fontFace = [[OS9Theme configuredFontName] copy] ?: @"Monaco";
        CGFloat sz = [OS9Theme configuredFontSize];
        _fontPt = (sz > 0 ? sz : 11);
    }
    [self msg:SCI_STYLESETFONT w:STYLE_DEFAULT l:(sptr_t)_fontFace.UTF8String];
    [self msg:SCI_STYLESETSIZE w:STYLE_DEFAULT l:(sptr_t)(long)_fontPt];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:STYLE_DEFAULT value:[NSColor blackColor]];
    [_sci setColorProperty:SCI_STYLESETBACK parameter:STYLE_DEFAULT value:[NSColor whiteColor]];
    [self msg:SCI_STYLECLEARALL w:0 l:0]; // apply default to all styles

    // line numbers: platinum background, gray text
    [_sci setColorProperty:SCI_STYLESETBACK parameter:STYLE_LINENUMBER value:[OS9Theme buttonFace]];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:STYLE_LINENUMBER value:[NSColor colorWithCalibratedWhite:0.4 alpha:1]];

    // Language-specific syntax colors. SCE_JSON_* and SCE_H_* SHARE style ids 0-13 -> apply ONLY the
    // active language's block (setting both would cross-color the other lexer's styles).
    if (_language == SciLanguageXml) [self applyXmlStyles];
    else [self applyJsonStyles];

    // caret + selection
    [_sci setColorProperty:SCI_SETCARETFORE parameter:0 value:[NSColor blackColor]];
    [_sci setColorProperty:SCI_SETSELBACK parameter:1 value:[NSColor colorWithCalibratedRed:0.78 green:0.82 blue:0.95 alpha:1]];
}

- (void)applyJsonStyles {
    NSColor *green = [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1];
    NSColor *blue = [NSColor colorWithCalibratedRed:0.1 green:0.2 blue:0.8 alpha:1];
    NSColor *purple = [NSColor colorWithCalibratedRed:0.45 green:0.1 blue:0.5 alpha:1];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_STRING value:green];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_STRINGEOL value:green];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_NUMBER value:blue];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_PROPERTYNAME value:purple];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_KEYWORD value:blue];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_JSON_OPERATOR value:[NSColor blackColor]];
}

- (void)applyXmlStyles {
    // Same palette family as JSON: tags purple (like property names), attributes blue, strings green.
    NSColor *green = [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.0 alpha:1];
    NSColor *blue = [NSColor colorWithCalibratedRed:0.1 green:0.2 blue:0.8 alpha:1];
    NSColor *purple = [NSColor colorWithCalibratedRed:0.45 green:0.1 blue:0.5 alpha:1];
    NSColor *gray = [NSColor colorWithCalibratedWhite:0.45 alpha:1];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_TAG value:purple];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_TAGUNKNOWN value:purple];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_TAGEND value:purple];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_XMLSTART value:gray];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_XMLEND value:gray];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_QUESTION value:gray];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_ATTRIBUTE value:blue];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_ATTRIBUTEUNKNOWN value:blue];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_DOUBLESTRING value:green];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_SINGLESTRING value:green];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_NUMBER value:blue];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_ENTITY value:blue];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_COMMENT value:gray];
    [_sci setColorProperty:SCI_STYLESETFORE parameter:SCE_H_CDATA value:gray];
}

- (void)setLanguage:(SciLanguage)lang {
    if (lang == _language) return; // idempotent — call sites fire on every tab switch/content set
    _language = lang;
    void *lexer = (lang == SciLanguageXml) ? DeedCreateXMLLexer() : DeedCreateJSONLexer();
    [self msg:SCI_SETILEXER w:0 l:(sptr_t)lexer]; // Scintilla releases the previous ILexer itself
    [self applyPlatinumTheme];                    // STYLECLEARALL + the new language's style block
    [_behavior applyHighlightStyles];             // brace-light styles were cleared -> re-set
    [self msg:SCI_COLOURISE w:0 l:(sptr_t)-1];    // restyle the current text under the new lexer
}

- (void)setFontName:(NSString *)name size:(CGFloat)size {
    if (name.length) _fontFace = [name copy];   // empty -> keep current font (applyPlatinumTheme uses ivar)
    if (size > 0) _fontPt = size;
    [self applyPlatinumTheme];                   // applies ivar font + STYLECLEARALL inside
    [_behavior applyHighlightStyles];            // STYLECLEARALL cleared them -> re-set brace colors
}

#pragma mark text get/set

- (NSString *)string { return [_sci string] ?: @""; }

- (void)setString:(NSString *)string {
    _programmatic = YES;
    [self msg:SCI_SETREADONLY w:0 l:0];            // unlock so we can set
    [_sci setString:string ?: @""];
    [self msg:SCI_SETREADONLY w:(_editable ? 0 : 1) l:0];
    [self msg:SCI_EMPTYUNDOBUFFER w:0 l:0];        // drop undo history -> don't keep the old text copy
    [self msg:SCI_SETSCROLLWIDTH w:1 l:0];         // reset scroll width
    [self msg:SCI_GOTOPOS w:0 l:0];
    _programmatic = NO;
}

- (void)clearContents {
    _programmatic = YES;
    [self msg:SCI_SETREADONLY w:0 l:0];
    [self msg:SCI_CLEARALL w:0 l:0];               // clear all text
    [self msg:SCI_SETREADONLY w:(_editable ? 0 : 1) l:0];
    [self msg:SCI_EMPTYUNDOBUFFER w:0 l:0];        // clear undo -> free the old content copy
    [self msg:SCI_SETSCROLLWIDTH w:1 l:0];
    _programmatic = NO;
}

#pragma mark Streaming render (SPEC_grpc_streaming §7)

// Is the last document line currently on screen? (decides whether to keep following the tail)
- (BOOL)isTailVisible {
    sptr_t first = [self q:SCI_GETFIRSTVISIBLELINE w:0 l:0];
    sptr_t onScreen = [self q:SCI_LINESONSCREEN w:0 l:0];
    sptr_t total = [self q:SCI_GETLINECOUNT w:0 l:0];
    return (first + onScreen) >= (total - 1);
}

- (void)scrollToEnd {
    sptr_t len = [self q:SCI_GETLENGTH w:0 l:0];
    [self msg:SCI_GOTOPOS w:(uptr_t)len l:0];
    [self msg:SCI_SCROLLCARET w:0 l:0];
}

- (void)beginStreaming {
    _streaming = YES;
    _followTail = YES;
    [self clearContents];           // resets text + undo + read-only to the configured state
    [self appendStreamChunk:@"[\n]"];   // seed a closed, valid (empty) array -> JSON is valid from frame 0
}

// Insert one chunk right before the trailing "\n]" so the array always stays closed/valid. The doc
// always ends in the 2-byte "\n]" (seeded by beginStreaming, preserved by every insert), so the insert
// position is length-2.
- (void)insertStreamChunk:(NSString *)chunk {
    if (!chunk.length) return;
    const char *utf8 = chunk.UTF8String;
    if (!utf8) return;
    sptr_t docLen = [self q:SCI_GETLENGTH w:0 l:0];
    sptr_t pos = docLen >= 2 ? docLen - 2 : 0;   // before the trailing "\n]"
    if (_followTail && ![self isTailVisible]) _followTail = NO;
    _programmatic = YES;
    [self msg:SCI_SETREADONLY w:0 l:0];
    [self msg:SCI_INSERTTEXT w:(uptr_t)pos l:(sptr_t)utf8];
    [self msg:SCI_SETREADONLY w:(_editable ? 0 : 1) l:0];
    _programmatic = NO;
    if (_followTail) [self scrollToEnd];
}

- (void)appendStreamChunk:(NSString *)chunk {
    if (!chunk.length) return;
    const char *utf8 = chunk.UTF8String;
    size_t len = strlen(utf8);
    // Decide follow BEFORE the append: if the user scrolled up, stop auto-scrolling (log-viewer behavior).
    if (_followTail && ![self isTailVisible]) _followTail = NO;
    _programmatic = YES;
    [self msg:SCI_SETREADONLY w:0 l:0];                       // read-only blocks APPENDTEXT -> unlock
    [self msg:SCI_APPENDTEXT w:(uptr_t)len l:(sptr_t)utf8];
    [self msg:SCI_SETREADONLY w:(_editable ? 0 : 1) l:0];     // re-lock
    _programmatic = NO;
    if (_followTail) [self scrollToEnd];
}

- (void)endStreamingValid:(BOOL)fold {
    _streaming = NO;
    [self msg:SCI_SETSCROLLWIDTH w:1 l:0];   // recompute horizontal extent for the final text
    if (fold) [self msg:SCI_GOTOPOS w:0 l:0];
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

// A click on the EMPTY area below the text lands on NSClipView (the document view is only
// as tall as the line count) -> never reaches SCIContentView -> doesn't grab focus. Redirect
// the hit to the content view so a click anywhere in the editor sets the caret + takes keyboard.
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
        case SCN_UPDATEUI:                           // (f) brace-match (no-op if _behavior is nil)
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
