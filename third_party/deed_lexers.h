// Shim: tạo JSON lexer của Lexilla cho SCI_SETILEXER mà không phơi nội bộ Lexilla ra app.
#ifndef DEED_LEXERS_H
#define DEED_LEXERS_H
#ifdef __cplusplus
extern "C" {
#endif
void *DeedCreateJSONLexer(void); // -> Scintilla::ILexer5* (ép kiểu void*)
#ifdef __cplusplus
}
#endif
#endif
