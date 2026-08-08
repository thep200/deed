#import "windows/typeui/RequestTypeUi.h"

#import "app/AppStrings.h"
#import "app/NsBridge.h"

#include "core/infra/serialization/field_json.hpp"

namespace d = core::domain;

@implementation GrpcTypeUi
- (core::RequestType)type { return core::RequestType::Grpc; }
- (BOOL)showsProtoPopup { return YES; }
- (BOOL)showsServicePopup { return YES; }
- (BOOL)offersCurlMenu { return YES; }
- (NSString *)displayName { return @"gRPC"; }
- (NSString *)newMenuTitle { return StrMenuNewGrpc; }
- (NSString *)defaultRequestName { return StrDefaultRpcName; }
- (NSString *)treeMark:(NSString *)methodOrType { return @"gRPC"; }

- (NSArray<NSString *> *)requestTabTitles:(const d::RequestModel &)m {
  // no Auth tab — call-level auth is a metadata entry (`authorization`); TLS is per-request Config
  return @[ StrTabMessage, StrTabMetadata, StrTabConfig ];
}
- (NSArray<NSString *> *)responseTabTitles:(const d::RequestModel &)m {
  return @[ StrTabMessage, StrTabRequest ];
}

- (EditorPlan *)populate:(const d::RequestModel &)m {
  const auto &g = std::get<d::GrpcRequest>(m.payload());
  EditorPlan *plan = [EditorPlan new];
  std::string msg = g.message().text();
  [plan.buffers addObject:N(msg.empty() ? "{}" : msg)];
  [plan.buffers addObject:N(core::serial::metadataToJson(g.metadata()))];
  plan.urlText = N(g.target());
  // reflection -> index 0; protoFiles/descriptorSet -> ".proto" (index 1)
  bool reflection = false;
  g.protoSource().match([&](auto &&p) {
    if constexpr (std::is_same_v<std::decay_t<decltype(p)>, d::ProtoReflection>) reflection = true;
  });
  plan.protoIndex = reflection ? 0 : 1;
  plan.wantsSavedRpcLabel = YES; // show the saved RPC (do NOT fetch; fetch on dropdown click)
  return plan;
}

- (std::optional<d::RequestModel::Payload>)payloadFromBuffers:(NSArray<NSString *> *)buffers
                                                          url:(const std::string &)url
                                                  methodTitle:(NSString *)methodTitle
                                                     bodyMode:(NSString *)bodyMode
                                                     oldModel:(const d::RequestModel &)cur
                                                         fail:(TypeUiSyncFail *)fail {
  const auto &curG = std::get<d::GrpcRequest>(cur.payload());
  d::GrpcRequest::Parts p;
  p.target = url;
  p.service = curG.service(); // service/method/methodType set via the RPC picker
  p.method = curG.method();
  p.methodType = curG.methodType();
  p.message = d::JsonText::of(S(buffers[0]));
  auto mr = core::serial::jsonToMetadata(S(buffers[1]));
  if (!mr.isOk()) {
    fail->tab = 1;
    fail->message = mr.error().message;
    return std::nullopt;
  }
  p.metadata = mr.take();
  p.protoSource = curG.protoSource();
  p.tls = curG.tls();
  auto gr = d::GrpcRequest::create(std::move(p));
  if (!gr.isOk()) {
    fail->tab = 0;
    fail->message = gr.error().message;
    return std::nullopt;
  }
  return d::RequestModel::Payload{gr.take()};
}

- (NSString *)importedName:(const d::RequestModel &)m {
  const auto &g = std::get<d::GrpcRequest>(m.payload());
  return g.method().empty() ? StrImportedGrpc : N(g.method());
}
- (NSString *)importSummary:(const d::RequestModel &)m {
  const auto &g = std::get<d::GrpcRequest>(m.payload());
  return [NSString stringWithFormat:@"target: %s\n%s / %s\nTLS: %@ · metadata: %lu",
                                    g.target().c_str(),
                                    g.service().empty() ? "(pick RPC)" : g.service().c_str(),
                                    g.method().c_str(),
                                    m.config().tlsEnabledDefault ? @"secure" : @"plaintext",
                                    (unsigned long)g.metadata().entries().size()];
}
@end
