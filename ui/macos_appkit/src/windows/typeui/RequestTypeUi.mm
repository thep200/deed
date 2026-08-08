#import "windows/typeui/RequestTypeUi.h"

#import "app/AppStrings.h"
#import "app/NsBridge.h"

@implementation EditorPlan
- (instancetype)init {
  if ((self = [super init])) {
    _buffers = [NSMutableArray array];
    _protoIndex = -1;
  }
  return self;
}
@end

@implementation RequestTypeUi
- (core::RequestType)type {
  NSAssert(NO, @"override");
  return core::RequestType::Http;
}
- (BOOL)showsMethodPopup { return NO; }
- (BOOL)showsProtoPopup { return NO; }
- (BOOL)showsServicePopup { return NO; }
- (BOOL)showsKafkaToggle { return NO; }
- (BOOL)offersCurlMenu { return NO; }
- (BOOL)usesKafkaConfigSerializer { return NO; }
- (NSString *)displayName { return N(core::toString([self type])); }
- (NSString *)newMenuTitle {
  NSAssert(NO, @"override");
  return @"";
}
- (NSString *)defaultRequestName { return StrDefaultRequestName; }
- (NSString *)treeMark:(NSString *)methodOrType { return methodOrType; }
- (NSString *)urlFieldText:(const core::domain::RequestModel &)m {
  std::string url;
  m.match([&](const auto &p) {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, core::domain::GrpcRequest>) url = p.target();
    else if constexpr (std::is_same_v<T, core::domain::KafkaRequest>)
      url = p.brokers().toBootstrapServers();
    else url = p.url().raw();
  });
  return N(url);
}
- (NSArray<NSString *> *)requestTabTitles:(const core::domain::RequestModel &)m {
  NSAssert(NO, @"override");
  return @[];
}
- (NSArray<NSString *> *)responseTabTitles:(const core::domain::RequestModel &)m {
  NSAssert(NO, @"override");
  return @[];
}
- (EditorPlan *)populate:(const core::domain::RequestModel &)m {
  NSAssert(NO, @"override");
  return [EditorPlan new];
}
- (std::optional<core::domain::RequestModel::Payload>)
    payloadFromBuffers:(NSArray<NSString *> *)buffers
                   url:(const std::string &)url
           methodTitle:(NSString *)methodTitle
              bodyMode:(NSString *)bodyMode
              oldModel:(const core::domain::RequestModel &)cur
                  fail:(TypeUiSyncFail *)fail {
  NSAssert(NO, @"override");
  return std::nullopt;
}
- (NSArray<NSString *> *)responseBuffers:(const core::domain::ApiResponse &)r
                                    body:(NSString *)renderedBody {
  return @[ renderedBody ?: @"", @"" ]; // body + empty Request tab
}
- (BOOL)statusLine:(const core::domain::ApiResponse &)r
              code:(NSString *__strong *)code
               bad:(BOOL *)bad {
  return NO;
}
- (NSString *)importedName:(const core::domain::RequestModel &)m { return N(m.name()); }
- (NSString *)importSummary:(const core::domain::RequestModel &)m {
  return [NSString stringWithFormat:@"%@", [self urlFieldText:m]];
}
@end

RequestTypeUi *TypeUiFor(core::RequestType t) {
  static NSArray<RequestTypeUi *> *byIndex;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    static_assert(core::kRequestTypeCount == 7, "add the new type's binder to BOTH arrays below");
    byIndex = @[
      [HttpTypeUi new], [GrpcTypeUi new], [GraphQlTypeUi new], [WsTypeUi new], [KafkaTypeUi new],
      [SoapTypeUi new], [LdapTypeUi new]
    ]; // index == RequestType enum value
  });
  NSUInteger i = (NSUInteger)t;
  return i < byIndex.count ? byIndex[i] : byIndex[0];
}

NSArray<RequestTypeUi *> *TypeUisInMenuOrder(void) {
  static NSArray<RequestTypeUi *> *order;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    order = @[
      TypeUiFor(core::RequestType::Http), TypeUiFor(core::RequestType::Grpc),
      TypeUiFor(core::RequestType::WebSocket), TypeUiFor(core::RequestType::GraphQl),
      TypeUiFor(core::RequestType::Kafka), TypeUiFor(core::RequestType::Soap),
      TypeUiFor(core::RequestType::Ldap)
    ];
  });
  return order;
}
