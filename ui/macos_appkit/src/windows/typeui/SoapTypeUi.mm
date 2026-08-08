#import "windows/typeui/RequestTypeUi.h"

#import "app/AppStrings.h"
#import "app/NsBridge.h"

#include "core/infra/serialization/field_json.hpp"

namespace d = core::domain;

@implementation SoapTypeUi
- (core::RequestType)type { return core::RequestType::Soap; }
- (NSString *)displayName { return @"SOAP"; }
- (NSString *)newMenuTitle { return StrMenuNewSoap; }
- (NSString *)defaultRequestName { return StrDefaultSoapName; }
- (NSString *)treeMark:(NSString *)methodOrType { return @"SOAP"; }

- (NSArray<NSString *> *)requestTabTitles:(const d::RequestModel &)m {
  return @[ StrTabEnvelope, StrTabHeaders, StrTabAuth, StrTabSoap, StrTabConfig ];
}
- (NSArray<NSString *> *)responseTabTitles:(const d::RequestModel &)m {
  // body + response headers (Content-Type separates a real <soap:Fault> from a proxy error page)
  return @[ StrTabResponse, StrTabHeaders ];
}

- (EditorPlan *)populate:(const d::RequestModel &)m {
  const auto &s = std::get<d::SoapRequest>(m.payload());
  EditorPlan *plan = [EditorPlan new];
  [plan.buffers addObject:N(s.envelope())];                             // 0 = Envelope (raw XML)
  [plan.buffers addObject:N(core::serial::headersToJson(s.headers()))]; // 1 = Headers
  [plan.buffers addObject:N(core::serial::authToJson(s.auth()))];       // 2 = Auth
  [plan.buffers addObject:N(core::serial::soapConfigToJson(s))];        // 3 = Soap {action,version}
  plan.urlText = N(s.url().raw());
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
  d::SoapRequest::Parts p{d::Url::create(url).take()};
  p.envelope = S(buffers[0]); // raw XML; emptiness checked at send time
  auto hr = core::serial::jsonToHeaders(S(buffers[1]));
  if (!hr.isOk()) return bail(1, hr.error().message);
  p.headers = hr.take();
  auto ar = core::serial::jsonToAuth(S(buffers[2]));
  if (!ar.isOk()) return bail(2, ar.error().message);
  p.auth = ar.take();
  auto sc = core::serial::jsonToSoapConfig(S(buffers[3]));
  if (!sc.isOk()) return bail(3, sc.error().message);
  p.action = sc.value().action;
  p.version = sc.value().version;
  auto sr = d::SoapRequest::create(std::move(p));
  if (!sr.isOk()) return bail(0, sr.error().message);
  return d::RequestModel::Payload{sr.take()};
}

- (NSArray<NSString *> *)responseBuffers:(const d::ApiResponse &)r body:(NSString *)renderedBody {
  return @[ renderedBody ?: @"", N(core::serial::responseHeadersToJson(r.headers)) ];
}
@end
