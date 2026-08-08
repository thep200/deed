// Thin ScintillaView wrapper hiding the SCI_* API; used for the editable request pane and read-only response.
#import <Cocoa/Cocoa.h>

// Lexer/style set for the pane. JSON is the default; XML is for the SOAP Envelope tab and XML-looking response bodies.
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
// Same language is a no-op. SCE_JSON_* and SCE_H_* share style IDs 0-13 -> styles re-applied per switch.
- (void)setLanguage:(SciLanguage)lang;
- (BOOL)hasFocus;                                     // is the editor holding the caret?
// Clear text + empty the undo buffer — call on request switch/close so old content isn't kept in RAM.
- (void)clearContents;

// Streaming render: beginStreaming seeds a VALID empty array "[\n]" (follow-tail on); insertStreamChunk
// inserts JUST BEFORE the trailing "]" so the JSON stays well-formed mid-stream; appendStreamChunk is a
// raw programmatic append (read-only toggled off/on); endStreamingValid leaves streaming (fold=YES may fold).
- (void)beginStreaming;
- (void)insertStreamChunk:(NSString *)chunk;
- (void)appendStreamChunk:(NSString *)chunk;
- (void)endStreamingValid:(BOOL)fold;
// Resign input context + detach the (unsafe_unretained) Scintilla delegate before destruction so
// notifications never hit a dead object. Idempotent; also runs in dealloc.
- (void)teardown;
@end
