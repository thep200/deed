// Self-drawn Platinum modal (no system NSAlert), synchronous; does NOT touch NSOpenPanel/NSSavePanel.
#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, OS9AlertIcon) {
    OS9AlertNone = 0,
    OS9AlertNote,
    OS9AlertCaution,
    OS9AlertStop,
};

@interface OS9Dialog : NSObject

// CONFIRM: returns the INDEX of the pressed button (0-based into the buttons array). Esc/Cmd-. -> cancelIdx.
+ (NSInteger)confirmWithTitle:(nullable NSString *)title
                      message:(nonnull NSString *)message
                      buttons:(nonnull NSArray<NSString *> *)buttons
                defaultButton:(NSInteger)defaultIdx
                 cancelButton:(NSInteger)cancelIdx
                         icon:(OS9AlertIcon)icon
                       parent:(nullable NSWindow *)parent;

// PROMPT (rename): returns the new (trimmed) text or nil if Cancel. validate returns nil if valid,
// or an error string -> dialog does NOT close, shows the error + keeps focus on the input field.
+ (nullable NSString *)promptWithTitle:(nonnull NSString *)title
                               message:(nullable NSString *)message
                           defaultText:(nonnull NSString *)text
                           placeholder:(nullable NSString *)placeholder
                              okButton:(nonnull NSString *)ok
                          cancelButton:(nonnull NSString *)cancel
                              validate:(NSString *_Nullable (^_Nullable)(NSString *_Nonnull input))validate
                                parent:(nullable NSWindow *)parent;
@end
