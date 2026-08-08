#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/domain/kafka/kafka_request.hpp"
#include "core/domain/ports/driven/i_clock.hpp"
#include "core/domain/ports/driven/i_json_validator.hpp"
#include "core/domain/ports/driven/i_request_observer.hpp"
#include "core/domain/ports/driven/i_request_sender.hpp"
#include "core/domain/ports/driven/i_response_cache.hpp"
#include "core/domain/ports/driven/i_token_provider.hpp"
#include "core/domain/request/request_model.hpp"

namespace sagatest {
using namespace core::domain;

struct FakeClock final : IClock {
  std::chrono::steady_clock::time_point now() const override {
    return std::chrono::steady_clock::time_point(std::chrono::milliseconds(1000));
  }
};

struct FakeObserver final : IRequestObserver {
  std::vector<ResponseEvent> events;
  void onEvent(RequestExecutionId, const ResponseEvent &ev) noexcept override { events.push_back(ev); }
};

struct FakeCache final : IResponseCache {
  int puts = 0;
  void put(const RequestId &, const ApiResponse &) override { ++puts; }
  std::optional<ApiResponse> get(const RequestId &) const override { return std::nullopt; }
};

struct FakeValidator final : IJsonValidator {
  bool ok = true;
  Status validate(const JsonText &) const override {
    return ok ? core::domain::ok() : Status::fail({ErrorCode::Parse, "bad json", "body.json"});
  }
};

struct FakeSender final : IRequestSender {
  RequestType type = RequestType::Http;
  std::vector<ResponseEvent> script;
  bool returnFail = false;
  bool executed = false;
  Auth seenAuth = Auth::none(); // what the saga handed us (asserts the OAuth2 -> Bearer rewrite)
  bool supports(RequestType t) const override { return t == type; }
  Status execute(const RequestModel &m, IResponseSink &sink, const ICancellationToken &) override {
    executed = true;
    if (m.type() == RequestType::Http) seenAuth = std::get<HttpRequest>(m.payload()).auth();
    for (const auto &ev : script) sink.emit(ev);
    return returnFail ? Status::fail({ErrorCode::Network, "boom"}) : core::domain::ok();
  }
};

struct FakeTokenProvider final : ITokenProvider {
  Result<std::string> result = Result<std::string>::ok("tok123");
  int calls = 0;
  Result<std::string> bearerFor(const AuthOAuth2 &, const Timeout &,
                                const ICancellationToken &) override {
    ++calls;
    return result;
  }
};

inline RequestModel makeHttp(Body body = Body::none()) {
  HttpRequest::Parts p{HttpMethod::Get, Url::create("https://h/x").take()};
  p.body = std::move(body);
  RequestConfig cfg{Timeout::fromMillis(1000).take(), true};
  return RequestModel::create(RequestId("req_1"), "R", 0, cfg, HttpRequest::create(std::move(p)).take())
      .take();
}

inline RequestModel makeHttpOAuth2() {
  AuthOAuth2 o;
  o.tokenUrl = "https://idp/token";
  o.clientId = "cid";
  HttpRequest::Parts p{HttpMethod::Get, Url::create("https://h/x").take()};
  p.auth = Auth::oauth2(std::move(o)).take();
  RequestConfig cfg{Timeout::fromMillis(1000).take(), true};
  return RequestModel::create(RequestId("req_o"), "O", 0, cfg, HttpRequest::create(std::move(p)).take())
      .take();
}

inline ResponseEvent completed() {
  ApiResponse r;
  r.statusCode = 200;
  r.body = "ok";
  return ResponseEvent(EvCompleted{r});
}

inline RequestModel makeKafkaProducer() {
  auto brokers = BrokerList::parse("localhost:9092").take();
  KafkaProduceConfig cfg{KafkaTopic::create("demo-topic").take()};
  KafkaMessage msg;
  msg.value = MessagePayload{"{}"};
  auto req = KafkaRequest::create(brokers, KafkaSecurity::plaintext(),
                                  KafkaRequest::Mode{KafkaProduceSpec{cfg, msg}})
                 .take();
  RequestConfig cfg2{Timeout::fromMillis(1800000).take(), false};
  return RequestModel::create(RequestId("req_kafka_p"), "P", 0, cfg2, req).take();
}

inline RequestModel makeKafkaConsumer() {
  auto brokers = BrokerList::parse("localhost:9092").take();
  KafkaConsumeConfig cfg{{KafkaTopic::create("demo-topic").take()}, std::nullopt,
                        ConsumerGroup::create("deed-tail-test").take()};
  auto req =
      KafkaRequest::create(brokers, KafkaSecurity::plaintext(), KafkaRequest::Mode{KafkaConsumeSpec{cfg}}).take();
  RequestConfig cfg2{Timeout::fromMillis(1800000).take(), false};
  return RequestModel::create(RequestId("req_kafka_c"), "C", 0, cfg2, req).take();
}

inline ResponseEvent kafkaRecord(int offset) {
  KafkaRecord r;
  r.topic = "demo-topic";
  r.partition = 0;
  r.offset = offset;
  r.value = "{}";
  return ResponseEvent(EvKafkaRecord{r});
}

template <class T> bool isAt(const std::vector<ResponseEvent> &v, size_t i) {
  return i < v.size() && v[i].is<T>();
}

} // namespace sagatest
