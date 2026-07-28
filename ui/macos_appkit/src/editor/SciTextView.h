// SciTextView — thin wrapper around ScintillaView (docs/RENDERING_AND_ASSETS.md §3.6).
// Hides the SCI_* API from the rest; used for both the request pane (editable) and response (read-only).
#import <Cocoa/Cocoa.h>

// Which lexer/style set the pane runs. JSON is the default (all tabs historically); XML is used for
// the SOAP Envelope tab and XML-looking response bodies (SPEC_soap highlight).
typedef NS_ENUM(NSInteger, SciLanguage) {
    SciLanguageJson,
    SciLanguageXml,
};

@interface SciTextView : NSView
@property(nonatomic, copy) NSString *string;          // get/set the entire text
@property(nonatomic) BOOL editable;                   // toggle editing (response = NO)
@property(nonatomic, copy) void (^onTextChanged)(void); // called when the USER edits (not when set by code)

- (instancetype)initEditable:(BOOL)editable;          // create + configure JSON + Platinum theme
- (void)setFontName:(NSString *)name size:(CGFloat)size;
// Switch the lexer + its style set (idempotent — same language is a no-op). JSON and XML share the
// same Scintilla style-ID space (SCE_JSON_* and SCE_H_* are both 0-13), so styles are re-applied
// per language on every switch.
- (void)setLanguage:(SciLanguage)lang;
- (BOOL)hasFocus;                                     // is the editor holding the caret?
// Free the text + undo buffers (LAZY_TREE §8.3): clear text then empty the undo buffer.
// Call when switching/closing a request so old content isn't kept in RAM.
- (void)clearContents;

// --- Streaming render (SPEC_grpc_streaming §7) ---
// beginStreaming: clear + seed an empty-but-VALID array "[\n]" so the JSON is well-formed from the very
//   first frame, then enter streaming-write mode (follow-tail on).
// insertStreamChunk: insert one coalesced chunk JUST BEFORE the trailing "]" -> the array stays valid the
//   whole time (the closing bracket is always present).
// appendStreamChunk: raw programmatic append at the end (used to seed); toggles read-only off/on.
// endStreamingValid: leave streaming mode; fold=YES may fold/validate the JSON array.
- (void)beginStreaming;
- (void)insertStreamChunk:(NSString *)chunk;
- (void)appendStreamChunk:(NSString *)chunk;
- (void)endStreamingValid:(BOOL)fold;
// Safe teardown (CRASH_FIX_LIFECYCLE §2.2): resign input context + detach the Scintilla delegate
// (unsafe_unretained) before destruction -> don't send notifications to a dead object. Idempotent;
// also called automatically in dealloc.
- (void)teardown;
@end
