#pragma once
#import <Cocoa/Cocoa.h>

#include <optional>
#include <string>

#include "core/domain/request/request_model.hpp"
#include "core/domain/response/api_response.hpp"

// Populate output — declarative; the controller applies it, binders never touch controller state.
@interface EditorPlan : NSObject
@property(nonatomic, strong) NSMutableArray<NSString *> *buffers; // one per request tab (Config appended by the controller)
@property(nonatomic, copy) NSString *urlText;
@property(nonatomic, copy) NSString *methodTitle;        // HTTP method popup selection; nil otherwise
@property(nonatomic, copy) NSString *bodyMode;           // HTTP: active body mode; controller overlays drafts on buffer 0
@property(nonatomic, copy) NSString *bodyActiveContent;  // HTTP: the model's body content for that mode
@property(nonatomic) NSInteger protoIndex;               // gRPC proto popup; -1 = leave alone
@property(nonatomic) BOOL wantsSavedRpcLabel;            // gRPC
@end

struct TypeUiSyncFail {
  NSInteger tab = 0; // -1 = URL field
  std::string message;
};

// Per-request-type UI behavior. One subclass per RequestType; TypeUiFor is the ONLY per-type table
// in the UI — everything else dispatches through it.
@interface RequestTypeUi : NSObject
- (core::RequestType)type;
// toolbar/layout flags
- (BOOL)showsMethodPopup;          // default NO
- (BOOL)showsProtoPopup;           // default NO
- (BOOL)showsServicePopup;         // default NO
- (BOOL)showsKafkaToggle;          // default NO
- (BOOL)offersCurlMenu;            // default NO
- (BOOL)usesKafkaConfigSerializer; // default NO
// naming / chrome
- (NSString *)displayName; // "HTTP", "gRPC", ... (toasts)
- (NSString *)newMenuTitle;
- (NSString *)defaultRequestName;
- (NSString *)treeMark:(NSString *)methodOrType; // tree badge; default = methodOrType (HTTP method)
// editor
- (NSString *)urlFieldText:(const core::domain::RequestModel &)m; // default = payload url()
- (NSArray<NSString *> *)requestTabTitles:(const core::domain::RequestModel &)m;
- (NSArray<NSString *> *)responseTabTitles:(const core::domain::RequestModel &)m;
- (EditorPlan *)populate:(const core::domain::RequestModel &)m;
- (std::optional<core::domain::RequestModel::Payload>)
    payloadFromBuffers:(NSArray<NSString *> *)buffers
                   url:(const std::string &)url
           methodTitle:(NSString *)methodTitle
              bodyMode:(NSString *)bodyMode
              oldModel:(const core::domain::RequestModel &)cur
                  fail:(TypeUiSyncFail *)fail;
// response pane
- (NSArray<NSString *> *)responseBuffers:(const core::domain::ApiResponse &)r
                                    body:(NSString *)renderedBody; // default = [body, ""] (Request tab)
- (BOOL)statusLine:(const core::domain::ApiResponse &)r
              code:(NSString *__strong *)code
               bad:(BOOL *)bad; // default NO = standard HTTP status handling
// import
- (NSString *)importedName:(const core::domain::RequestModel &)m; // default = model name
- (NSString *)importSummary:(const core::domain::RequestModel &)m; // default = url line
@end

@interface HttpTypeUi : RequestTypeUi
@end
@interface GrpcTypeUi : RequestTypeUi
@end
@interface GraphQlTypeUi : RequestTypeUi
@end
@interface WsTypeUi : RequestTypeUi
@end
@interface KafkaTypeUi : RequestTypeUi
@end
@interface SoapTypeUi : RequestTypeUi
@end
@interface LdapTypeUi : RequestTypeUi
@end

// Registry: index == RequestType enum value.
RequestTypeUi *TypeUiFor(core::RequestType t);
// The "New <type>" menu display order (NOT enum order — keep the menu the user sees today).
NSArray<RequestTypeUi *> *TypeUisInMenuOrder(void);
