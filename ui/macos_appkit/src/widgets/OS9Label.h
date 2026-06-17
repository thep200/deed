// Nhãn nhỏ kiểu OS9 (NSTextField không viền) + biến thể căn giữa dọc.
#import <Cocoa/Cocoa.h>

// Nhãn nhỏ kiểu OS9 (NSTextField không viền).
NSTextField *OS9Label(NSString *text);

// Như OS9Label nhưng CĂN GIỮA theo chiều dọc (dùng cho status line cao hơn dòng chữ).
NSTextField *OS9CenteredLabel(NSString *text);
