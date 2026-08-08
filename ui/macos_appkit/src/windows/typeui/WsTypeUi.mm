#import "windows/typeui/RequestTypeUi.h"

#import "app/AppStrings.h"
#import "app/NsBridge.h"

#include "core/infra/serialization/field_json.hpp"

namespace d = core::domain;

@implementation WsTypeUi
- (core::RequestType)type { return core::RequestType::WebSocket; }
- (NSString *)displayName { return @"WebSocket"; }
- (NSString *)newMenuTitle { return StrMenuNewWs; }
- (NSString *)defaultRequestName { return StrDefaultWsName; }
- (NSString *)treeMark:(NSString *)methodOrType { return @"WS"; }

- (NSArray<NSString *> *)requestTabTitles:(const d::RequestModel &)m {
  // Message = frame to send (also auto-sent on connect); Headers = handshake headers
  return @[ StrTabMessage, StrTabHeaders, StrTabAuth, StrTabConfig ];
}
- (NSArray<NSString *> *)responseTabTitles:(const d::RequestModel &)m {
  return @[ StrTabMessage, StrTabRequest ]; // in/out frame log (streaming render)
}

- (EditorPlan *)populate:(const d::RequestModel &)m {
  const auto &w = std::get<d::WebSocketRequest>(m.payload());
  EditorPlan *plan = [EditorPlan new];
  std::string frame = w.onOpenSend().empty() ? std::string() : w.onOpenSend()[0].payload;
  [plan.buffers addObject:N(frame)];                                    // 0 = Message
  [plan.buffers addObject:N(core::serial::headersToJson(w.headers()))]; // 1 = Headers
  [plan.buffers addObject:N(core::serial::authToJson(w.auth()))];       // 2 = Auth
  plan.urlText = N(w.url().raw());
  return plan;
}

- (std::optional<d::RequestModel::Payload>)payloadFromBuffers:(NSArray<NSString *> *)buffers
                                                          url:(const std::string &)url
                                                  methodTitle:(NSString *)methodTitle
                                                     bodyMode:(NSString *)bodyMode
                                                     oldModel:(const d::RequestModel &)cur
                                                         fail:(TypeUiSyncFail *)fail {
  auto bail = [&](NSInteger tab, const std::string &e) -> std::optional<d::RequestModel::Payload> {
    fail->tab = tab;
    fail->message = e;
    return std::nullopt;
  };
  const auto &curW = std::get<d::WebSocketRequest>(cur.payload());
  d::WebSocketRequest::Parts p{d::Url::create(url).take()};
  p.subprotocols = curW.subprotocols();
  p.defaultSendKind = curW.defaultSendKind();
  std::string frame = S(buffers[0]);
  if (!frame.empty()) p.onOpenSend.push_back({d::WsSendKind::Text, frame});
  auto hr = core::serial::jsonToHeaders(S(buffers[1]));
  if (!hr.isOk()) return bail(1, hr.error().message);
  p.headers = hr.take();
  auto ar = core::serial::jsonToAuth(S(buffers[2]));
  if (!ar.isOk()) return bail(2, ar.error().message);
  p.auth = ar.take();
  auto wr = d::WebSocketRequest::create(std::move(p));
  if (!wr.isOk()) return bail(0, wr.error().message);
  return d::RequestModel::Payload{wr.take()};
}
@end
