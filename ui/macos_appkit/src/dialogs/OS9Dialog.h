// OS9Dialog — popup modal tự vẽ kiểu Platinum (thay NSAlert hệ thống). CUSTOM_DIALOG.md.
// Hai API đồng bộ: confirm (trả index nút) và prompt (trả text mới hoặc nil).
// Reuse OS9Theme/OS9BevelButton; KHÔNG đụng NSOpenPanel/NSSavePanel.
#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, OS9AlertIcon) {
    OS9AlertNone = 0,
    OS9AlertNote,
    OS9AlertCaution,
    OS9AlertStop,
};

@interface OS9Dialog : NSObject

// CONFIRM: trả về INDEX nút được bấm (0-based theo mảng buttons). Esc/Cmd-. -> cancelIdx.
+ (NSInteger)confirmWithTitle:(nullable NSString *)title
                      message:(nonnull NSString *)message
                      buttons:(nonnull NSArray<NSString *> *)buttons
                defaultButton:(NSInteger)defaultIdx
                 cancelButton:(NSInteger)cancelIdx
                         icon:(OS9AlertIcon)icon
                       parent:(nullable NSWindow *)parent;

// PROMPT (rename): trả text mới (đã trim) hoặc nil nếu Cancel. validate trả nil nếu hợp lệ,
// hoặc chuỗi lỗi -> dialog KHÔNG đóng, hiện lỗi + giữ focus ô input.
+ (nullable NSString *)promptWithTitle:(nonnull NSString *)title
                               message:(nullable NSString *)message
                           defaultText:(nonnull NSString *)text
                           placeholder:(nullable NSString *)placeholder
                              okButton:(nonnull NSString *)ok
                          cancelButton:(nonnull NSString *)cancel
                              validate:(nullable NSString *(^)(NSString *_Nonnull input))validate
                                parent:(nullable NSWindow *)parent;
@end
