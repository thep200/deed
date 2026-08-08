#import "windows/typeui/RequestTypeUi.h"

#import "app/AppStrings.h"
#import "app/NsBridge.h"

#include "core/infra/serialization/field_json.hpp"

namespace d = core::domain;

@implementation GraphQlTypeUi
- (core::RequestType)type { return core::RequestType::GraphQl; }
- (NSString *)displayName { return @"GraphQL"; }
- (NSString *)newMenuTitle { return StrMenuNewGraphQl; }
- (NSString *)defaultRequestName { return StrDefaultGqlName; }
- (NSString *)treeMark:(NSString *)methodOrType { return @"GQL"; }

- (NSArray<NSString *> *)requestTabTitles:(const d::RequestModel &)m {
  return @[ StrTabGqlQuery, StrTabVariables, StrTabHeaders, StrTabAuth, StrTabConfig ];
}
- (NSArray<NSString *> *)responseTabTitles:(const d::RequestModel &)m {
  // Schema tab is NOT buffer-backed (content in the controller's _gqlSchema* state); keep it LAST so
  // the response-buffer clamp falls back to Response.
  return @[ StrTabResponse, StrTabRequest, StrTabSchema ];
}

- (EditorPlan *)populate:(const d::RequestModel &)m {
  const auto &g = std::get<d::GraphQlRequest>(m.payload());
  EditorPlan *plan = [EditorPlan new];
  std::string vars = g.op().variables.text();
  [plan.buffers addObject:N(g.op().query)];                             // 0 = Query document
  [plan.buffers addObject:N(vars.empty() ? "{}" : vars)];               // 1 = Variables (JSON)
  [plan.buffers addObject:N(core::serial::headersToJson(g.headers()))]; // 2 = Headers
  [plan.buffers addObject:N(core::serial::authToJson(g.auth()))];       // 3 = Auth
  plan.urlText = N(g.url().raw());
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
  const auto &curG = std::get<d::GraphQlRequest>(cur.payload());
  d::GraphQlRequest::Parts p{d::Url::create(url).take()};
  p.op = curG.op(); // preserve operation type / operationName (not edited here)
  p.op.query = S(buffers[0]);
  p.op.variables = d::JsonText::of(S(buffers[1]));
  p.subTransport = curG.subTransport();
  p.wsProtocol = curG.wsProtocol();
  auto hr = core::serial::jsonToHeaders(S(buffers[2]));
  if (!hr.isOk()) return bail(2, hr.error().message);
  p.headers = hr.take();
  auto ar = core::serial::jsonToAuth(S(buffers[3]));
  if (!ar.isOk()) return bail(3, ar.error().message);
  p.auth = ar.take();
  auto gr = d::GraphQlRequest::create(std::move(p));
  if (!gr.isOk()) return bail(0, gr.error().message);
  return d::RequestModel::Payload{gr.take()};
}

- (NSString *)importedName:(const d::RequestModel &)m {
  const auto &g = std::get<d::GraphQlRequest>(m.payload());
  return g.op().operationName.empty() ? N(m.name()) : N(g.op().operationName);
}
- (NSString *)importSummary:(const d::RequestModel &)m {
  const auto &g = std::get<d::GraphQlRequest>(m.payload());
  NSMutableString *s = [NSMutableString string];
  if (!g.url().raw().empty()) [s appendFormat:@"endpoint: %s\n", g.url().raw().c_str()];
  NSString *firstLine = [N(g.op().query) componentsSeparatedByString:@"\n"].firstObject ?: @"";
  [s appendFormat:@"%@\n", firstLine];
  [s appendFormat:@"headers: %lu", (unsigned long)g.headers().size()];
  return s;
}
@end
