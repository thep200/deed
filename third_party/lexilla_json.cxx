// Tạo JSON lexer trực tiếp từ LexerModule (không qua Lexilla.cxx — vốn tham chiếu MỌI lexer).
#include "ILexer.h"
#include "LexerModule.h"
#include "deed_lexers.h"

// lmJSON định nghĩa trong lexers/LexJSON.cxx ở global namespace, kiểu Lexilla::LexerModule.
extern Lexilla::LexerModule lmJSON;

extern "C" void *DeedCreateJSONLexer(void) {
    return static_cast<void *>(lmJSON.Create());
}
