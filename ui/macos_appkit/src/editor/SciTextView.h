// SciTextView — wrapper mỏng quanh ScintillaView (docs/RENDERING_AND_ASSETS.md §3.6).
// Giấu API SCI_* khỏi phần còn lại; dùng cho cả pane request (sửa được) lẫn response (read-only).
#import <Cocoa/Cocoa.h>

@interface SciTextView : NSView
@property(nonatomic, copy) NSString *string;          // get/set toàn bộ text
@property(nonatomic) BOOL editable;                   // bật/tắt sửa (response = NO)
@property(nonatomic, copy) void (^onTextChanged)(void); // gọi khi NGƯỜI DÙNG sửa (không gọi khi set bằng code)
// Mode comment cho Ctrl+/ / Cmd+/ (json/graphql/xml/text/grpc...). nil/"" = TẮT toggle
// (vd pane không phải Body/Message). Marker lấy từ core::codec::commentMarkerFor. SPEC §T7.
@property(nonatomic, copy) NSString *commentMode;

- (instancetype)initEditable:(BOOL)editable;          // tạo + cấu hình JSON + theme Platinum
// Toggle comment cho dòng/khối đang chọn theo commentMode (1 lần undo). No-op nếu commentMode rỗng.
- (void)toggleCommentSelection;
- (void)setFontName:(NSString *)name size:(CGFloat)size;
- (BOOL)hasFocus;                                     // editor đang giữ con trỏ?
// Giải phóng buffer text + undo (LAZY_TREE §8.3): xoá text rồi empty undo buffer.
// Gọi khi chuyển/đóng request để không giữ nội dung cũ trong RAM.
- (void)clearContents;
// Teardown an toàn (CRASH_FIX_LIFECYCLE §2.2): resign input context + gỡ delegate Scintilla
// (unsafe_unretained) trước khi huỷ -> không gửi notification tới object đã chết. Idempotent;
// cũng được gọi tự động trong dealloc.
- (void)teardown;
@end
