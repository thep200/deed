#import "windows/typeui/RequestTypeUi.h"

#import "app/AppStrings.h"
#import "app/NsBridge.h"

#include "core/infra/serialization/field_json.hpp"

namespace d = core::domain;

@implementation HttpTypeUi
- (core::RequestType)type { return core::RequestType::Http; }
- (BOOL)showsMethodPopup { return YES; }
- (BOOL)offersCurlMenu { return YES; }
- (NSString *)displayName { return @"HTTP"; }
- (NSString *)newMenuTitle { return StrMenuNewHttp; }
- (NSString *)defaultRequestName { return StrDefaultRequestName; }

- (NSArray<NSString *> *)requestTabTitles:(const d::RequestModel &)m {
  // "Query" (avoid confusion with path params); Body leftmost, Config last for every type
  return @[ StrTabBody, StrTabQuery, StrTabHeaders, StrTabAuth, StrTabConfig ];
}
- (NSArray<NSString *> *)responseTabTitles:(const d::RequestModel &)m {
  return @[ StrTabResponse, StrTabHeaders, StrTabRequest, StrTabCookie ];
}

- (EditorPlan *)populate:(const d::RequestModel &)m {
  const auto &h = std::get<d::HttpRequest>(m.payload());
  EditorPlan *plan = [EditorPlan new];
  // Body: the domain Body holds ONE mode; decompose to (mode, content). The controller overlays its
  // per-mode drafts on buffer 0 (populate can't be pure here — drafts are UI state).
  core::serial::EditorBody eb = core::serial::bodyToEditor(h.body());
  plan.bodyMode = N(eb.mode);
  plan.bodyActiveContent = eb.content.empty() ? nil : N(eb.content);
  [plan.buffers addObject:@""];                                        // 0 = Body (controller fills)
  [plan.buffers addObject:N(core::serial::paramsToJson(h.params()))];  // 1 = Params
  [plan.buffers addObject:N(core::serial::headersToJson(h.headers()))]; // 2 = Headers
  [plan.buffers addObject:N(core::serial::authToJson(h.auth()))];      // 3 = Auth
  plan.methodTitle = N(d::toString(h.method()));
  plan.urlText = N(h.url().raw());
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
  auto mr = d::parseHttpMethod(S(methodTitle ?: @"GET"));
  d::HttpMethod method = mr.isOk() ? mr.take() : d::HttpMethod::Get;
  d::HttpRequest::Parts p{method, d::Url::create(url).take()};
  auto br = core::serial::bodyFromEditor(S(bodyMode.length ? bodyMode : @"json"), S(buffers[0]));
  if (!br.isOk()) return bail(0, br.error().message);
  p.body = br.take();
  auto pr = core::serial::jsonToParams(S(buffers[1]));
  if (!pr.isOk()) return bail(1, pr.error().message);
  p.params = pr.take();
  auto hr = core::serial::jsonToHeaders(S(buffers[2]));
  if (!hr.isOk()) return bail(2, hr.error().message);
  p.headers = hr.take();
  auto ar = core::serial::jsonToAuth(S(buffers[3]));
  if (!ar.isOk()) return bail(3, ar.error().message);
  p.auth = ar.take();
  return d::RequestModel::Payload{d::HttpRequest::create(std::move(p)).take()};
}

- (NSArray<NSString *> *)responseBuffers:(const d::ApiResponse &)r body:(NSString *)renderedBody {
  NSMutableArray<NSString *> *bufs = [NSMutableArray array];
  [bufs addObject:renderedBody ?: @""];
  [bufs addObject:N(core::serial::responseHeadersToJson(r.headers))];
  [bufs addObject:@""]; // Request tab (resolved request) — not carried by ApiResponse
  NSMutableString *ck = [NSMutableString string];
  for (const auto &c : r.cookies)
    [ck appendFormat:@"%s=%s  (domain=%s path=%s expires=%s)\n", c.name.c_str(), c.value.c_str(),
                     c.domain.c_str(), c.path.c_str(), c.expires.c_str()];
  [bufs addObject:(ck.length ? ck : StrNoSetCookie)];
  return bufs;
}

- (NSString *)importedName:(const d::RequestModel &)m {
  const auto &h = std::get<d::HttpRequest>(m.payload());
  NSString *path = N(h.url().raw());
  NSRange q = [path rangeOfString:@"?"];
  if (q.location != NSNotFound) path = [path substringToIndex:q.location];
  NSString *last = path.lastPathComponent;
  if (!last.length || [last containsString:@":"]) last = StrDefaultImportName; // host only
  return [NSString stringWithFormat:@"%s %@", d::toString(h.method()).c_str(), last];
}
- (NSString *)importSummary:(const d::RequestModel &)m {
  const auto &h = std::get<d::HttpRequest>(m.payload());
  return [NSString stringWithFormat:@"%s  %s\nheaders: %lu", d::toString(h.method()).c_str(),
                                    h.url().raw().c_str(), (unsigned long)h.headers().size()];
}
@end
