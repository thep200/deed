#include "ILexer.h"
#include "LexerModule.h"
#include "deed_lexers.h"

extern Lexilla::LexerModule lmJSON;

extern "C" void *DeedCreateJSONLexer(void) {
    return static_cast<void *>(lmJSON.Create());
}
