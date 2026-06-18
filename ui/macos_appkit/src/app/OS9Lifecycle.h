// OS9Lifecycle — hợp đồng teardown first-responder / input-context (docs/CRASH_FIX_LIFECYCLE.md §2).
//
// Crash gốc: một text view/field còn là first responder bị xoá nội dung/huỷ hoặc window
// chứa nó bị đóng MÀ KHÔNG deactivate NSTextInputContext trước -> vòng sự kiện kế tiếp
// -[NSApplication updateWindows] kích hoạt lại input context đã free -> EXC_BAD_ACCESS
// (nặng hơn khi gõ IME tiếng Việt vì đường IMK/TSM bám lâu hơn).
//
// Quy tắc bất biến: TRƯỚC khi huỷ/đổi/xoá text view/field, hoặc đóng window chứa text field
// -> gọi OS9SafeEndEditing trong khi object còn SỐNG để commit + nhả input context sạch.
#import <Cocoa/Cocoa.h>

// Goi TRUOC khi go/replace text view, hoac truoc khi dong window co text field.
// dyingViewOrNil == nil  -> resign first responder vô điều kiện (đóng window / xoá nhiều view).
// dyingViewOrNil != nil  -> chỉ resign nếu first responder LÀ view đó (hoặc con của nó).
static inline void OS9SafeEndEditing(NSWindow *w, NSView *dyingViewOrNil) {
    if (!w) return;
    [w endEditingFor:nil];                 // commit + nhả field editor (NSTextField)
    id fr = w.firstResponder;
    BOOL frIsDying = (dyingViewOrNil && (fr == dyingViewOrNil ||
                       ([fr isKindOfClass:NSView.class] && [(NSView *)fr isDescendantOf:dyingViewOrNil])));
    if (dyingViewOrNil == nil || frIsDying) {
        [w makeFirstResponder:nil];        // deactivate NSTextInputContext của view sắp chết
    }
}
