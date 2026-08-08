#import "windows/typeui/RequestTypeUi.h"

#import "app/AppStrings.h"
#import "app/NsBridge.h"

#include "core/infra/serialization/field_json.hpp"

namespace d = core::domain;

@implementation LdapTypeUi
- (core::RequestType)type { return core::RequestType::Ldap; }
- (NSString *)displayName { return @"LDAP"; }
- (NSString *)newMenuTitle { return StrMenuNewLdap; }
- (NSString *)defaultRequestName { return StrDefaultLdapName; }
- (NSString *)treeMark:(NSString *)methodOrType { return @"LDAP"; }

- (NSArray<NSString *> *)requestTabTitles:(const d::RequestModel &)m {
  // ONE Params tab (bind/base/filter/group/limits JSON — LDAP has no Headers/Auth/Body) + Config
  return @[ StrTabLdapParams, StrTabConfig ];
}
- (NSArray<NSString *> *)responseTabTitles:(const d::RequestModel &)m {
  return @[ StrTabResponse ]; // verdict JSON only; LDAP has no response headers
}

- (EditorPlan *)populate:(const d::RequestModel &)m {
  const auto &l = std::get<d::LdapRequest>(m.payload());
  EditorPlan *plan = [EditorPlan new];
  [plan.buffers addObject:N(core::serial::ldapParamsToJson(l))]; // 0 = Params
  plan.urlText = N(l.url().raw());
  return plan;
}

- (std::optional<d::RequestModel::Payload>)payloadFromBuffers:(NSArray<NSString *> *)buffers
                                                          url:(const std::string &)url
                                                  methodTitle:(NSString *)methodTitle
                                                     bodyMode:(NSString *)bodyMode
                                                     oldModel:(const d::RequestModel &)cur
                                                         fail:(TypeUiSyncFail *)fail {
  auto pr = core::serial::jsonToLdapParams(S(buffers[0]));
  if (!pr.isOk()) {
    fail->tab = 0;
    fail->message = pr.error().message;
    return std::nullopt;
  }
  const auto &q = pr.value();
  d::LdapRequest::Parts p{d::Url::create(url).take()};
  p.startTls = q.startTls;
  p.bindDn = q.bindDn;
  p.bindPassword = q.bindPassword;
  p.baseDn = q.baseDn;
  p.scope = q.scope;
  p.filter = q.filter;
  p.attributes = q.attributes;
  p.group = q.group;
  p.testPassword = q.testPassword;
  p.sizeLimit = q.sizeLimit;
  p.timeLimit = q.timeLimit;
  p.pageSize = q.pageSize;
  auto lr = d::LdapRequest::create(std::move(p));
  if (!lr.isOk()) {
    fail->tab = 0;
    fail->message = lr.error().message;
    return std::nullopt;
  }
  return d::RequestModel::Payload{lr.take()};
}

- (NSArray<NSString *> *)responseBuffers:(const d::ApiResponse &)r body:(NSString *)renderedBody {
  return @[ renderedBody ?: @"" ]; // no Request tab; body = verdict JSON
}

- (BOOL)statusLine:(const d::ApiResponse &)r code:(NSString *__strong *)code bad:(BOOL *)bad {
  // statusCode is an LDAP result code, not HTTP — "rc=N · VERDICT (matched)", colored by verdict
  // (only MATCH/CREDENTIALS_OK are green; rc=49 is < 400 but IS a red answer).
  NSData *bd = [[NSData alloc] initWithBytes:r.body.data() length:r.body.size()];
  id j = bd.length ? [NSJSONSerialization JSONObjectWithData:bd options:0 error:nil] : nil;
  NSString *verdict = [j isKindOfClass:NSDictionary.class] ? (j[@"verdict"] ?: @"") : @"";
  if (!verdict.length) return NO;
  *code = [NSString stringWithFormat:@"rc=%d · %@ (%@)", r.statusCode, verdict, j[@"matched"] ?: @0];
  *bad = !([verdict isEqualToString:@"MATCH"] || [verdict isEqualToString:@"CREDENTIALS_OK"]);
  return YES;
}
@end
