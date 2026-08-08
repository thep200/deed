#import "windows/typeui/RequestTypeUi.h"

#import "app/AppStrings.h"
#import "app/NsBridge.h"

#include "core/infra/serialization/field_json.hpp"

namespace d = core::domain;

@implementation KafkaTypeUi
- (core::RequestType)type { return core::RequestType::Kafka; }
- (BOOL)showsKafkaToggle { return YES; }
- (BOOL)usesKafkaConfigSerializer { return YES; }
- (NSString *)displayName { return @"Kafka"; }
- (NSString *)newMenuTitle { return StrMenuNewKafka; }
- (NSString *)defaultRequestName { return StrDefaultKafkaName; }
- (NSString *)treeMark:(NSString *)methodOrType { return @"KAFKA"; }

- (NSArray<NSString *> *)requestTabTitles:(const d::RequestModel &)m {
  // Producer: Message + Kafka config. Consumer: ONE Kafka tab (nothing to compose). Config last.
  const auto &k = std::get<d::KafkaRequest>(m.payload());
  if (k.kind() == d::KafkaClientKind::Consumer) return @[ StrTabKafkaConfig, StrTabConfig ];
  return @[ StrTabMessage, StrTabKafkaConfig, StrTabConfig ];
}
- (NSArray<NSString *> *)responseTabTitles:(const d::RequestModel &)m {
  const auto &k = std::get<d::KafkaRequest>(m.payload());
  if (k.kind() == d::KafkaClientKind::Consumer) return @[ StrTabMessage ]; // streaming record log
  return @[ StrTabResponse ];                                              // one delivery report
}

- (EditorPlan *)populate:(const d::RequestModel &)m {
  const auto &k = std::get<d::KafkaRequest>(m.payload());
  EditorPlan *plan = [EditorPlan new];
  if (k.kind() == d::KafkaClientKind::Producer) {
    const auto &p = std::get<d::KafkaProduceSpec>(k.mode());
    [plan.buffers addObject:N(core::serial::kafkaMessageToJson(p.message))];      // 0 = Message
    [plan.buffers addObject:N(core::serial::kafkaProduceConfigToJson(p.config))]; // 1 = Kafka config
  } else {
    const auto &c = std::get<d::KafkaConsumeSpec>(k.mode());
    [plan.buffers addObject:N(core::serial::kafkaConsumeConfigToJson(c.config))]; // 0 = Kafka config
  }
  plan.urlText = N(k.brokers().toBootstrapServers());
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
  const auto &curK = std::get<d::KafkaRequest>(cur.payload());
  auto br = d::BrokerList::parse(url);
  if (!br.isOk()) return bail(-1, br.error().message); // -1: reported on the URL field
  // curK.inactiveDraft() rides along unchanged: the editors only hold the ACTIVE kind's tabs, and
  // dropping the draft here would lose the other side on every autosave.
  if (curK.kind() == d::KafkaClientKind::Producer) {
    auto msgR = core::serial::jsonToKafkaMessage(S(buffers[0]));
    if (!msgR.isOk()) return bail(0, msgR.error().message);
    auto cfgR = core::serial::jsonToKafkaProduceConfig(S(buffers[1]));
    if (!cfgR.isOk()) return bail(1, cfgR.error().message);
    auto kr = d::KafkaRequest::create(br.take(), curK.security(),
                                      d::KafkaRequest::Mode{d::KafkaProduceSpec{cfgR.take(), msgR.take()}},
                                      curK.inactiveDraft());
    if (!kr.isOk()) return bail(0, kr.error().message);
    return d::RequestModel::Payload{kr.take()};
  }
  auto cfgR = core::serial::jsonToKafkaConsumeConfig(S(buffers[0]));
  if (!cfgR.isOk()) return bail(0, cfgR.error().message);
  auto kr = d::KafkaRequest::create(br.take(), curK.security(),
                                    d::KafkaRequest::Mode{d::KafkaConsumeSpec{cfgR.take()}},
                                    curK.inactiveDraft());
  if (!kr.isOk()) return bail(0, kr.error().message);
  return d::RequestModel::Payload{kr.take()};
}

- (NSArray<NSString *> *)responseBuffers:(const d::ApiResponse &)r body:(NSString *)renderedBody {
  return @[ renderedBody ?: @"" ]; // no Request tab
}
@end
