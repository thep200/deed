// XML lexer factory shim (SPEC_soap highlight): lmXML lives in LexHTML.cxx (SCLEX_XML is the
// "secondary" lexer of the HTML family). Same pattern as lexilla_json.cxx.
#include "ILexer.h"
#include "LexerModule.h"
#include "deed_lexers.h"

extern Lexilla::LexerModule lmXML;

extern "C" void *DeedCreateXMLLexer(void) {
    return static_cast<void *>(lmXML.Create());
}
