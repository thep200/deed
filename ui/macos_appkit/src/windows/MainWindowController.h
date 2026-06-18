#import <Cocoa/Cocoa.h>

#import "bridge/CoreBridge.h"

// Cửa sổ chính: cây (1)(2), editor JSON-thô (3), response (4), ENV (5), method (6),
// URL/target (7), status (8), proto (9), send (10), setting (11). Xem UI spec §5.
// Controller tách thành nhiều file category (+Tree/+Editor/+Send/+Config/+Stress) cho gọn.
// Conformance protocol đặt ở ĐÚNG category cài đặt nó (khai trong MainWindowController+Private.h):
//   NSOutlineView* -> +Tree, NSTextField/TextView -> +Editor, CoreResponseSink -> +Send.
// Primary chỉ giữ NSWindowDelegate (cài trong MainWindowController.mm) -> tránh cảnh báo
// -Wincomplete-implementation / -Wobjc-protocol-method-implementation.
@interface MainWindowController : NSObject <NSWindowDelegate>
- (void)showWindow;
@end

// Action public (gọi từ AppController/menu). Cài ở category nên khai báo riêng ngoài primary
// interface để clang không đòi cài trong @implementation chính.
@interface MainWindowController (Actions)
- (void)openCollectionRoot:(NSString *)path; // mở collection theo path (dev/CI affordance)
- (void)openFolder:(id)sender;
- (void)saveRequest:(id)sender;
- (void)sendRequest:(id)sender;
- (void)copyAsCurl:(id)sender;
@end
